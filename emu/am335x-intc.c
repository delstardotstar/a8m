/*
 * AM335x interrupt controller (INTCPS, TRM chapter 6): 128 level inputs,
 * per-line priority and IRQ/FIQ routing, mask and software-set banks.
 * Sorting is recomputed on every change rather than latched until NEWIRQAGR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "am335x.h"

#define NBANK 4
#define NIRQ (NBANK * 32)

OBJECT_DECLARE_SIMPLE_TYPE(A8IntcState, A8_INTC)

struct A8IntcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq fiq;
    uint32_t level[NBANK];
    uint32_t isr[NBANK];
    uint32_t mir[NBANK];
    uint32_t ilr[NIRQ];
    uint32_t sysconfig;
    uint32_t threshold;
    uint32_t protection;
    uint32_t idle;
    bool fiq_ok;
};

/* Highest-priority (lowest ILR priority value) pending line for IRQ or FIQ. */
static int a8_intc_best(A8IntcState *s, bool fiq, unsigned *prio)
{
    int best = -1;
    unsigned bp = 64;

    for (int b = 0; b < NBANK; b++) {
        uint32_t p = (s->level[b] | s->isr[b]) & ~s->mir[b];
        while (p) {
            int bit = ctz32(p);
            int n = b * 32 + bit;
            unsigned pr = (s->ilr[n] >> 2) & 0x3f;
            p &= p - 1;
            if ((s->ilr[n] & 1) != fiq) {
                continue;
            }
            if (pr < bp) {
                bp = pr;
                best = n;
            }
        }
    }
    if (prio) {
        *prio = bp;
    }
    return best;
}

static void a8_intc_update(A8IntcState *s)
{
    unsigned th = s->threshold & 0xff;
    unsigned pr;
    int n;

    n = a8_intc_best(s, false, &pr);
    qemu_set_irq(s->irq, n >= 0 && (th == 0xff || pr < th));
    n = a8_intc_best(s, true, &pr);
    qemu_set_irq(s->fiq, n >= 0 && (th == 0xff || pr < th));
}

/* The module hangs on FIQ (the boot ROM appears to give it to the secure
 * monitor), so stop as soon as a line routed to FIQ is unmasked. */
static void a8_intc_check_fiq(A8IntcState *s)
{
    for (int n = 0; n < NIRQ && !s->fiq_ok; n++) {
        if ((s->ilr[n] & 1) && !(s->mir[n / 32] & (1u << (n % 32)))) {
            error_report("a8: interrupt %d routed to FIQ and unmasked; FIQ "
                         "hangs the A8 -- halting (A8_HWCHECK=0 allows it)", n);
            exit(3);
        }
    }
}

static void a8_intc_set(void *opaque, int n, int level)
{
    A8IntcState *s = opaque;

    if (level) {
        s->level[n / 32] |= 1u << (n % 32);
    } else {
        s->level[n / 32] &= ~(1u << (n % 32));
    }
    a8_intc_update(s);
}

static void a8_intc_reset_regs(A8IntcState *s)
{
    memset(s->isr, 0, sizeof(s->isr));
    memset(s->ilr, 0, sizeof(s->ilr));
    for (int b = 0; b < NBANK; b++) {
        s->mir[b] = 0xffffffff;
    }
    s->sysconfig = 0;
    s->threshold = 0xff;
    s->protection = 0;
    s->idle = 0;
}

static uint64_t a8_intc_read(void *opaque, hwaddr offset, unsigned size)
{
    A8IntcState *s = opaque;
    unsigned pr;
    int n;

    if (offset >= 0x80 && offset < 0x80 + NBANK * 0x20) {
        int b = (offset - 0x80) / 0x20;
        switch ((offset - 0x80) % 0x20) {
        case 0x00: return s->level[b] | s->isr[b];
        case 0x04: return s->mir[b];
        case 0x10: return s->isr[b];
        case 0x18: {
            uint32_t p = (s->level[b] | s->isr[b]) & ~s->mir[b];
            uint32_t f = 0;
            for (int i = 0; i < 32; i++) {
                f |= (s->ilr[b * 32 + i] & 1u) << i;
            }
            return p & ~f;
        }
        case 0x1c: {
            uint32_t p = (s->level[b] | s->isr[b]) & ~s->mir[b];
            uint32_t f = 0;
            for (int i = 0; i < 32; i++) {
                f |= (s->ilr[b * 32 + i] & 1u) << i;
            }
            return p & f;
        }
        default: return 0;
        }
    }
    if (offset >= 0x100 && offset < 0x100 + NIRQ * 4) {
        return s->ilr[(offset - 0x100) / 4];
    }
    switch (offset) {
    case 0x00: return 0x50;
    case 0x10: return s->sysconfig;
    case 0x14: return 1;
    case 0x40:
    case 0x44:
        n = a8_intc_best(s, offset == 0x44, &pr);
        return n < 0 ? 0xffffff80 : (uint32_t)n;
    case 0x48: return 0;
    case 0x4c: return s->protection;
    case 0x50: return s->idle;
    case 0x60:
    case 0x64:
        n = a8_intc_best(s, offset == 0x64, &pr);
        return n < 0 ? 0xffffffc0 : pr;
    case 0x68: return s->threshold;
    }
    qemu_log_mask(LOG_UNIMP, "intc: read 0x%03" HWADDR_PRIx "\n", offset);
    return 0;
}

static void a8_intc_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    A8IntcState *s = opaque;
    uint32_t v = value;

    if (offset >= 0x80 && offset < 0x80 + NBANK * 0x20) {
        int b = (offset - 0x80) / 0x20;
        switch ((offset - 0x80) % 0x20) {
        case 0x04: s->mir[b] = v; break;
        case 0x08: s->mir[b] &= ~v; break;
        case 0x0c: s->mir[b] |= v; break;
        case 0x10: s->isr[b] |= v; break;
        case 0x14: s->isr[b] &= ~v; break;
        }
        a8_intc_check_fiq(s);
        a8_intc_update(s);
        return;
    }
    if (offset >= 0x100 && offset < 0x100 + NIRQ * 4) {
        s->ilr[(offset - 0x100) / 4] = v & 0xfd;
        a8_intc_check_fiq(s);
        a8_intc_update(s);
        return;
    }
    switch (offset) {
    case 0x10:
        if (v & 2) {
            a8_intc_reset_regs(s);
        } else {
            s->sysconfig = v & 1;
        }
        break;
    case 0x48: break;
    case 0x4c: s->protection = v & 1; break;
    case 0x50: s->idle = v & 3; break;
    case 0x68: s->threshold = v & 0xff; break;
    default:
        qemu_log_mask(LOG_UNIMP, "intc: write 0x%03" HWADDR_PRIx
                      " <- 0x%08x\n", offset, v);
        return;
    }
    a8_intc_update(s);
}

static const MemoryRegionOps a8_intc_ops = {
    .read = a8_intc_read,
    .write = a8_intc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void a8_intc_init(Object *obj)
{
    A8IntcState *s = A8_INTC(obj);

    memory_region_init_io(&s->iomem, obj, &a8_intc_ops, s, "intc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->fiq);
    qdev_init_gpio_in(DEVICE(obj), a8_intc_set, NIRQ);
    s->fiq_ok = !g_strcmp0(g_getenv("A8_HWCHECK"), "0");
    a8_intc_reset_regs(s);
}

static const TypeInfo a8_intc_info = {
    .name = TYPE_A8_INTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8IntcState),
    .instance_init = a8_intc_init,
};

static void a8_intc_register(void)
{
    type_register_static(&a8_intc_info);
}
type_init(a8_intc_register)
