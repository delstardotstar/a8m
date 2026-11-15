/*
 * AM335x WDT1 (TRM chapter 20.4): 32-bit up-counter on the 32 kHz clock,
 * reloaded from WLDR by a new value in WTGR, started and stopped with the
 * WSPR sequences (BBBB/4444, AAAA/5555). Overflow resets the SoC: QEMU
 * exits with A8_EXIT_WDT and emu/run.sh boots again with PRM_RSTST's
 * WDT1_RST bit set. Starts stopped (MLO stops it before app runs).
 * Writes are never posted (WWPS reads 0). A8_WDT=0 logs the overflow
 * instead of resetting.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "system/runstate.h"
#include "am335x.h"

#define WDT_HZ 32768u

OBJECT_DECLARE_SIMPLE_TYPE(A8WdtState, A8_WDT)

struct A8WdtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    bool running;
    uint32_t wclr, wldr, wtgr, wspr, wdly, wdsc;
    uint32_t base_count;
    int64_t base_ns;
};

static uint64_t a8_wdt_hz(A8WdtState *s)
{
    uint64_t hz = WDT_HZ / a8_timescale;

    return s->wclr & 0x20 ? hz >> ((s->wclr >> 2) & 7) : hz;
}

static uint32_t a8_wdt_count(A8WdtState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t c;

    if (!s->running) {
        return s->base_count;
    }
    c = s->base_count + muldiv64(now - s->base_ns, a8_wdt_hz(s), NANOSECONDS_PER_SECOND);
    return c > 0xffffffffull ? 0xffffffffu : (uint32_t)c;
}

static void a8_wdt_arm(A8WdtState *s)
{
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (!s->running) {
        timer_del(s->timer);
        return;
    }
    timer_mod(s->timer, s->base_ns + muldiv64(0x100000000ull - s->base_count,
                                              NANOSECONDS_PER_SECOND, a8_wdt_hz(s)));
}

static void a8_wdt_expire(void *opaque)
{
    A8WdtState *s = opaque;

    if (!s->running) {
        return;
    }
    if (!g_strcmp0(g_getenv("A8_WDT"), "0")) {
        fprintf(stderr, "a8: WDT1 overflow ignored (A8_WDT=0)\n");
        s->base_count = s->wldr;
        a8_wdt_arm(s);
        return;
    }
    fprintf(stderr, "a8: WDT1 reset\n");
    s->running = false;
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_RESET, A8_EXIT_WDT);
}

static uint64_t a8_wdt_read(void *opaque, hwaddr off, unsigned size)
{
    A8WdtState *s = opaque;

    switch (off) {
    case 0x00: return 0x50201511;
    case 0x10: return s->wdsc;
    case 0x14: return 1;            /* WDST: reset done */
    case 0x24: return s->wclr;
    case 0x28: return a8_wdt_count(s);
    case 0x2c: return s->wldr;
    case 0x30: return s->wtgr;
    case 0x34: return 0;            /* WWPS: nothing pending */
    case 0x44: return s->wdly;
    case 0x48: return s->wspr;
    }
    return 0;
}

static void a8_wdt_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    A8WdtState *s = opaque;
    uint32_t v = value;

    switch (off) {
    case 0x10:
        s->wdsc = v & ~2u;
        return;
    case 0x24:
        s->base_count = a8_wdt_count(s);
        s->wclr = v & 0x3c;
        a8_wdt_arm(s);
        return;
    case 0x28:
        s->base_count = v;
        a8_wdt_arm(s);
        return;
    case 0x2c:
        s->wldr = v;
        return;
    case 0x30:
        if (v != s->wtgr) {
            s->base_count = s->wldr;
            a8_wdt_arm(s);
        }
        s->wtgr = v;
        return;
    case 0x44:
        s->wdly = v;
        return;
    case 0x48:
        if ((s->wspr == 0xbbbb && v == 0x4444) || (s->wspr == 0xaaaa && v == 0x5555)) {
            s->base_count = a8_wdt_count(s);
            s->running = v == 0x4444;
            fprintf(stderr, "a8: WDT1 %s\n", s->running ? "started" : "stopped");
            a8_wdt_arm(s);
        }
        s->wspr = v;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "wdt1: write 0x%02" HWADDR_PRIx " <- 0x%08x\n", off, v);
}

static const MemoryRegionOps a8_wdt_ops = {
    .read = a8_wdt_read,
    .write = a8_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void a8_wdt_realize(DeviceState *dev, Error **errp)
{
    A8WdtState *s = A8_WDT(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_wdt_ops, s, "wdt1", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a8_wdt_expire, s);
    s->wclr = 0x20;
    s->wldr = s->base_count = 0xfffb0000u;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void a8_wdt_class_init(ObjectClass *oc, const void *data)
{
    DEVICE_CLASS(oc)->realize = a8_wdt_realize;
}

static const TypeInfo a8_wdt_info = {
    .name = TYPE_A8_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8WdtState),
    .class_init = a8_wdt_class_init,
};

static void a8_wdt_register(void)
{
    type_register_static(&a8_wdt_info);
}
type_init(a8_wdt_register)
