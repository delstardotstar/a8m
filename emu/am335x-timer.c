/*
 * AM335x DMTimer (TRM chapter 20): 32-bit up-counter with load, auto-reload,
 * compare and prescaler; overflow and match interrupts. Capture and PWM
 * outputs are not modelled. Writes are never posted (TWPS reads 0).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "am335x.h"

#define TCLR_ST  (1u << 0)
#define TCLR_AR  (1u << 1)
#define TCLR_PRE (1u << 5)
#define TCLR_CE  (1u << 6)

#define IRQ_MAT 1u
#define IRQ_OVF 2u

OBJECT_DECLARE_SIMPLE_TYPE(A8TimerState, A8_TIMER)

struct A8TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *timer;
    uint32_t freq;
    uint32_t tiocp, raw, enable, wakeup, tclr, tldr, tmar, tsicr;
    uint32_t base_count;
    int64_t base_ns;
};

static uint64_t a8_timer_hz(A8TimerState *s)
{
    uint64_t hz = s->freq / a8_timescale;

    if (s->tclr & TCLR_PRE) {
        return hz >> (((s->tclr >> 2) & 7) + 1);
    }
    return hz;
}

static uint64_t a8_timer_elapsed(A8TimerState *s, int64_t now)
{
    if (!(s->tclr & TCLR_ST)) {
        return 0;
    }
    return muldiv64(now - s->base_ns, a8_timer_hz(s), NANOSECONDS_PER_SECOND);
}

static void a8_timer_update_irq(A8TimerState *s)
{
    qemu_set_irq(s->irq, !!(s->raw & s->enable));
}

/*
 * Advance the counter to now, latching overflow/match status, then arm the
 * QEMU timer for the next event.
 */
static void a8_timer_sync(A8TimerState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t ticks = a8_timer_elapsed(s, now);
    uint64_t c = s->base_count;
    uint64_t next = UINT64_MAX;

    if (s->tclr & TCLR_ST) {
        uint64_t to_ovf = 0x100000000ull - c;
        if ((s->tclr & TCLR_CE) && s->tmar >= c && ticks >= s->tmar - c) {
            s->raw |= IRQ_MAT;
        }
        if (ticks >= to_ovf) {
            s->raw |= IRQ_OVF;
            if (s->tclr & TCLR_AR) {
                uint64_t period = 0x100000000ull - s->tldr;
                c = s->tldr + (ticks - to_ovf) % period;
                if ((s->tclr & TCLR_CE) && s->tmar < c) {
                    s->raw |= IRQ_MAT;
                }
            } else {
                c = 0;
                s->tclr &= ~TCLR_ST;
            }
        } else {
            c += ticks;
        }
        /* Rebase on whole ticks so fractional time carries over. */
        s->base_ns += muldiv64(ticks, NANOSECONDS_PER_SECOND, a8_timer_hz(s));
        s->base_count = c;

        if (s->tclr & TCLR_ST) {
            next = 0x100000000ull - c;
            if ((s->tclr & TCLR_CE) && s->tmar > c) {
                next = MIN(next, s->tmar - c);
            }
        }
    }

    if (next == UINT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, s->base_ns +
                  muldiv64(next, NANOSECONDS_PER_SECOND, a8_timer_hz(s)));
    }
    a8_timer_update_irq(s);
}

static void a8_timer_tick(void *opaque)
{
    a8_timer_sync(opaque);
}

static void a8_timer_reset(A8TimerState *s)
{
    s->tiocp = s->raw = s->enable = s->wakeup = 0;
    s->tclr = s->tldr = s->tmar = s->tsicr = 0;
    s->base_count = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_del(s->timer);
    a8_timer_update_irq(s);
}

static uint64_t a8_timer_read(void *opaque, hwaddr off, unsigned size)
{
    A8TimerState *s = opaque;

    switch (off) {
    case 0x00: return 0x40000100;
    case 0x10: return s->tiocp;
    case 0x20: return 0;
    case 0x24: a8_timer_sync(s); return s->raw;
    case 0x28: a8_timer_sync(s); return s->raw & s->enable;
    case 0x2c:
    case 0x30: return s->enable;
    case 0x34: return s->wakeup;
    case 0x38: return s->tclr;
    case 0x3c: a8_timer_sync(s); return s->base_count;
    case 0x40: return s->tldr;
    case 0x44: return 0;
    case 0x48: return 0;
    case 0x4c: return s->tmar;
    case 0x50: return 0;
    case 0x54: return s->tsicr;
    case 0x58: return 0;
    }
    qemu_log_mask(LOG_UNIMP, "dmtimer: read 0x%02" HWADDR_PRIx "\n", off);
    return 0;
}

static void a8_timer_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    A8TimerState *s = opaque;
    uint32_t v = value;

    a8_timer_sync(s);
    switch (off) {
    case 0x10:
        if (v & 1) {
            a8_timer_reset(s);
            return;
        }
        s->tiocp = v;
        return;
    case 0x20: break;
    case 0x24: s->raw |= v & 7; break;
    case 0x28: s->raw &= ~v; break;
    case 0x2c: s->enable |= v & 7; break;
    case 0x30: s->enable &= ~v; break;
    case 0x34: s->wakeup = v & 7; return;
    case 0x38: s->tclr = v & 0x1ffff; break;
    case 0x3c: s->base_count = v; break;
    case 0x40: s->tldr = v; return;
    case 0x44: s->base_count = s->tldr; break;
    case 0x4c: s->tmar = v; break;
    case 0x54: s->tsicr = v & 6; return;
    default:
        qemu_log_mask(LOG_UNIMP, "dmtimer: write 0x%02" HWADDR_PRIx
                      " <- 0x%08x\n", off, v);
        return;
    }
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    a8_timer_sync(s);
}

static const MemoryRegionOps a8_timer_ops = {
    .read = a8_timer_read,
    .write = a8_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void a8_timer_realize(DeviceState *dev, Error **errp)
{
    A8TimerState *s = A8_TIMER(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &a8_timer_ops, s,
                          "dmtimer", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a8_timer_tick, s);
    a8_timer_reset(s);
}

static const Property a8_timer_props[] = {
    DEFINE_PROP_UINT32("freq", A8TimerState, freq, 24000000),
};

static void a8_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = a8_timer_realize;
    device_class_set_props(dc, a8_timer_props);
}

static const TypeInfo a8_timer_info = {
    .name = TYPE_A8_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8TimerState),
    .class_init = a8_timer_class_init,
};

static void a8_timer_register(void)
{
    type_register_static(&a8_timer_info);
}
type_init(a8_timer_register)
