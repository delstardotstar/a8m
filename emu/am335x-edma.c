/*
 * AM335x EDMA3 channel controller (TRM chapter 11), DMA channels only.
 * Transfers complete instantly in a bottom half; transfer controllers, queues,
 * QDMA and region-specific interrupt routing are not modelled (one completion
 * IRQ for IPR & IER).
 * Every DDR range a transfer reads or writes goes to the hook the uncached
 * plugin publishes in A8_DMA_HOOK (its cache model: the EDMA sees DDR, not
 * what the CPU's caches hold).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "am335x.h"

#define NCH 64
#define NPARAM 256
#define PARAM_BASE 0x4000

#define OPT_SAM     (1u << 0)
#define OPT_DAM     (1u << 1)
#define OPT_SYNCDIM (1u << 2)
#define OPT_STATIC  (1u << 3)
#define OPT_TCC(o)  (((o) >> 12) & 0x3f)
#define OPT_TCINTEN   (1u << 20)
#define OPT_ITCINTEN  (1u << 21)
#define OPT_TCCHEN    (1u << 22)
#define OPT_ITCCHEN   (1u << 23)

OBJECT_DECLARE_SIMPLE_TYPE(A8EdmaState, A8_EDMA)

typedef struct {
    uint32_t opt, src, abcnt, dst, bidx, link, cidx, ccnt;
} A8Param;

struct A8EdmaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUBH *bh;
    uint32_t dchmap[NCH];
    uint32_t dmaqnum[8];
    uint32_t drae[8][2];
    uint64_t er, eer, ser, cer, ier, ipr, emr;
    uint32_t ccerr;
    uint32_t qchmap[8];
    uint32_t qeer, qer;
    A8Param param[NPARAM];
};

static void a8_edma_update(A8EdmaState *s)
{
    qemu_set_irq(s->irq, !!(s->ipr & s->ier));
}

typedef void DmaHook(int write, uint64_t pa, uint32_t n);

static DmaHook *dma_hook(void)
{
    static DmaHook *hook;
    static bool done;

    if (!done) {
        const char *v = g_getenv("A8_DMA_HOOK");

        done = true;
        hook = v ? (DmaHook *)(uintptr_t)g_ascii_strtoull(v, NULL, 16) : NULL;
    }
    return hook;
}

static void a8_edma_copy(A8EdmaState *s, uint32_t opt, uint32_t src,
                         uint32_t dst, uint32_t n)
{
    uint8_t buf[0x10000];
    DmaHook *hook = dma_hook();

    if (hook) {
        hook(0, src, opt & OPT_SAM ? MIN(n, 4) : n);
        hook(1, dst, opt & OPT_DAM ? MIN(n, 4) : n);
    }

    if (opt & OPT_SAM) {
        for (uint32_t i = 0; i < n; i += 4) {
            address_space_read(&address_space_memory, src,
                               MEMTXATTRS_UNSPECIFIED, buf + i, MIN(4, n - i));
        }
    } else {
        address_space_read(&address_space_memory, src,
                           MEMTXATTRS_UNSPECIFIED, buf, n);
    }
    if (opt & OPT_DAM) {
        for (uint32_t i = 0; i < n; i += 4) {
            address_space_write(&address_space_memory, dst,
                                MEMTXATTRS_UNSPECIFIED, buf + i,
                                MIN(4, n - i));
        }
    } else {
        address_space_write(&address_space_memory, dst,
                            MEMTXATTRS_UNSPECIFIED, buf, n);
    }
}

static void a8_edma_tc(A8EdmaState *s, uint32_t opt, bool final)
{
    int tcc = OPT_TCC(opt);

    if (opt & (final ? OPT_TCINTEN : OPT_ITCINTEN)) {
        s->ipr |= 1ull << tcc;
    }
    if (opt & (final ? OPT_TCCHEN : OPT_ITCCHEN)) {
        s->cer |= 1ull << tcc;
        qemu_bh_schedule(s->bh);
    }
}

/*
 * One synchronization event on PaRAM set pn. Returns true when the set
 * completed and linked to a non-null set (QDMA re-triggers on that).
 */
static bool a8_edma_sync(A8EdmaState *s, int pn, const char *what, int ch)
{
    A8Param *p = &s->param[pn];
    uint32_t acnt = p->abcnt & 0xffff;
    uint32_t bcnt = p->abcnt >> 16;
    uint32_t ccnt = p->ccnt & 0xffff;
    int16_t sbidx = p->bidx, dbidx = p->bidx >> 16;
    int16_t scidx = p->cidx, dcidx = p->cidx >> 16;
    bool final;

    if (!acnt || !bcnt || !ccnt) {
        qemu_log_mask(LOG_GUEST_ERROR, "edma: null event on %s %d (param %d)\n",
                      what, ch, pn);
        return false;
    }

    if (p->opt & OPT_SYNCDIM) {
        uint32_t src = p->src, dst = p->dst;
        for (uint32_t b = 0; b < bcnt; b++) {
            a8_edma_copy(s, p->opt, src, dst, acnt);
            src += sbidx;
            dst += dbidx;
        }
        final = ccnt == 1;
        if (!(p->opt & OPT_STATIC)) {
            p->src += scidx;
            p->dst += dcidx;
            p->ccnt = (p->ccnt & ~0xffff) | (ccnt - 1);
        }
    } else {
        a8_edma_copy(s, p->opt, p->src, p->dst, acnt);
        final = bcnt == 1 && ccnt == 1;
        if (!(p->opt & OPT_STATIC)) {
            if (bcnt > 1) {
                p->src += sbidx;
                p->dst += dbidx;
                p->abcnt = acnt | ((bcnt - 1) << 16);
            } else {
                p->src += scidx;
                p->dst += dcidx;
                p->abcnt = acnt | (p->link & 0xffff0000);
                p->ccnt = (p->ccnt & ~0xffff) | (ccnt - 1);
            }
        }
    }

    a8_edma_tc(s, p->opt, final);
    if (final && !(p->opt & OPT_STATIC)) {
        uint16_t link = p->link & 0xffff;
        if (link == 0xffff) {
            p->abcnt = 0;
            p->ccnt &= ~0xffff;
            return false;
        }
        /* Only the PaRAM offset bits count: 0x4480 and 0x0480 both mean set 36. */
        *p = s->param[(link & (NPARAM * 32 - 1)) / 32];
        return (p->abcnt & 0xffff) && (p->abcnt >> 16) && (p->ccnt & 0xffff);
    }
    return false;
}

static void a8_edma_run(void *opaque)
{
    A8EdmaState *s = opaque;
    uint64_t todo;

    for (;;) {
        if ((todo = (s->er & s->eer) | s->cer | s->ser)) {
            int ch = ctz64(todo);
            uint64_t bit = 1ull << ch;

            s->er &= ~bit;
            s->cer &= ~bit;
            s->ser &= ~bit;
            a8_edma_sync(s, (s->dchmap[ch] >> 5) & (NPARAM - 1), "ch", ch);
        } else if (s->qer & s->qeer) {
            int q = ctz32(s->qer & s->qeer);

            s->qer &= ~(1u << q);
            if (a8_edma_sync(s, (s->qchmap[q] >> 5) & (NPARAM - 1), "qdma", q)) {
                s->qer |= 1u << q;
            }
        } else {
            break;
        }
    }
    a8_edma_update(s);
}

void a8_edma_event(DeviceState *dev, int ev)
{
    A8EdmaState *s = A8_EDMA(dev);
    uint64_t bit = 1ull << ev;

    if (s->er & bit) {
        s->emr |= bit;
    }
    s->er |= bit;
    if (s->eer & bit) {
        qemu_bh_schedule(s->bh);
    }
}

/* Raise an event and run it before returning (for synchronous peripherals). */
void a8_edma_event_now(DeviceState *dev, int ev)
{
    A8EdmaState *s = A8_EDMA(dev);

    a8_edma_event(dev, ev);
    if (s->eer & (1ull << ev)) {
        a8_edma_run(s);
    }
}

static uint32_t *a8_edma_param_word(A8EdmaState *s, hwaddr off)
{
    return &((uint32_t *)&s->param[(off - PARAM_BASE) / 32])[(off % 32) / 4];
}

static uint32_t half(uint64_t v, bool hi)
{
    return hi ? v >> 32 : (uint32_t)v;
}

static void set_half(uint64_t *v, bool hi, uint32_t x)
{
    *v = hi ? (*v & 0xffffffffull) | ((uint64_t)x << 32)
            : (*v & ~0xffffffffull) | x;
}

static uint64_t a8_edma_read32(A8EdmaState *s, hwaddr off);
static void a8_edma_write32(A8EdmaState *s, hwaddr off, uint32_t v);

/* app patches PaRAM ACNT/BCNT with 16-bit stores; merge narrow accesses. */
static uint64_t a8_edma_read(void *opaque, hwaddr off, unsigned size)
{
    uint32_t w = a8_edma_read32(opaque, off & ~3);

    return extract32(w, (off & 3) * 8, size * 8);
}

static void a8_edma_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    A8EdmaState *s = opaque;
    hwaddr base = off & ~3;
    uint32_t v = value;

    if (size < 4) {
        if (base < PARAM_BASE || base >= PARAM_BASE + NPARAM * 32) {
            qemu_log_mask(LOG_GUEST_ERROR, "edma: %u-byte write to 0x%04"
                          HWADDR_PRIx "\n", size, off);
            return;
        }
        v = deposit32(*a8_edma_param_word(s, base), (off & 3) * 8, size * 8,
                      value);
    }
    a8_edma_write32(s, base, v);
}

static uint64_t a8_edma_read32(A8EdmaState *s, hwaddr off)
{
    if (off >= PARAM_BASE && off < PARAM_BASE + NPARAM * 32) {
        return *a8_edma_param_word(s, off);
    }
    if (off >= 0x100 && off < 0x200) {
        return s->dchmap[(off - 0x100) / 4];
    }
    if (off >= 0x240 && off < 0x260) {
        return s->dmaqnum[(off - 0x240) / 4];
    }
    if (off >= 0x200 && off < 0x220) {
        return s->qchmap[(off - 0x200) / 4];
    }
    if (off >= 0x340 && off < 0x380) {
        return s->drae[(off - 0x340) / 8][((off - 0x340) / 4) & 1];
    }
    if (off >= 0x1000 && off < 0x3000) {
        hwaddr r = off < 0x2000 ? off - 0x1000 : (off - 0x2000) % 0x200;
        bool hi = r & 4;
        switch (r & ~4) {
        case 0x00: return half(s->er, hi);
        case 0x20: return half(s->eer, hi);
        case 0x38: return half(s->ser, hi);
        case 0x50: return half(s->ier, hi);
        case 0x68: return half(s->ipr, hi);
        default:
            if (r == 0x80) return s->qer;
            if (r == 0x84) return s->qeer;
            return 0;
        }
    }
    switch (off) {
    case 0x000: return 0x40014c00;
    case 0x004: return 0x00224035;
    case 0x300: return half(s->emr, false);
    case 0x304: return half(s->emr, true);
    case 0x318: return s->ccerr;
    case 0x640: return 0;
    }
    qemu_log_mask(LOG_UNIMP, "edma: read 0x%04" HWADDR_PRIx "\n", off);
    return 0;
}

static void a8_edma_write32(A8EdmaState *s, hwaddr off, uint32_t v)
{
    if (off >= PARAM_BASE && off < PARAM_BASE + NPARAM * 32) {
        int pn = (off - PARAM_BASE) / 32;
        int word = (off % 32) / 4;

        *a8_edma_param_word(s, off) = v;
        /* Trigger writes latch in QER even while the channel is disabled. */
        for (int q = 0; q < 8; q++) {
            if (((s->qchmap[q] >> 5) & (NPARAM - 1)) == pn &&
                ((s->qchmap[q] >> 2) & 7) == word) {
                s->qer |= 1u << q;
                qemu_bh_schedule(s->bh);
            }
        }
        return;
    }
    if (off >= 0x100 && off < 0x200) {
        s->dchmap[(off - 0x100) / 4] = v & 0x3fe0;
        return;
    }
    if (off >= 0x240 && off < 0x260) {
        s->dmaqnum[(off - 0x240) / 4] = v;
        return;
    }
    if (off >= 0x340 && off < 0x380) {
        s->drae[(off - 0x340) / 8][((off - 0x340) / 4) & 1] = v;
        return;
    }
    if (off >= 0x1000 && off < 0x3000) {
        hwaddr r = off < 0x2000 ? off - 0x1000 : (off - 0x2000) % 0x200;
        bool hi = r & 4;
        uint64_t m = hi ? (uint64_t)v << 32 : v;
        switch (r & ~4) {
        case 0x08: s->er &= ~m; break;
        case 0x10: s->er |= m; s->ser |= m & ~s->eer; break;
        case 0x20: set_half(&s->eer, hi, v); break;
        case 0x28: s->eer &= ~m; break;
        case 0x30: s->eer |= m; break;
        case 0x40: s->ser &= ~m; break;
        case 0x50: set_half(&s->ier, hi, v); break;
        case 0x58: s->ier &= ~m; break;
        case 0x60: s->ier |= m; break;
        case 0x70: s->ipr &= ~m; break;
        case 0x78: break;
        case 0x80: case 0x84: case 0x88: case 0x8c: case 0x90: case 0x94:
            if (hi) {
                switch (r) {
                case 0x84: s->qeer = v; break;
                case 0x8c: s->qeer |= v; break;
                case 0x94: break;
                }
            } else {
                switch (r) {
                case 0x80: break;
                case 0x88: s->qeer &= ~v; break;
                case 0x90: break;
                }
            }
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "edma: write 0x%04" HWADDR_PRIx
                          " <- 0x%08x\n", off, v);
            return;
        }
        /* IPR/IER changes reach the IRQ line at once, as on hardware: an
         * ISR that clears its bit must not be re-entered before the BH. */
        a8_edma_update(s);
        qemu_bh_schedule(s->bh);
        return;
    }
    switch (off) {
    case 0x308: s->emr &= ~(uint64_t)v; return;
    case 0x30c: s->emr &= ~((uint64_t)v << 32); return;
    case 0x31c: s->ccerr &= ~v; return;
    case 0x200: case 0x204: case 0x208: case 0x20c:
    case 0x210: case 0x214: case 0x218: case 0x21c:
        s->qchmap[(off - 0x200) / 4] = v & 0x3ffc;
        return;
    case 0x310: case 0x314: case 0x320: case 0x284: case 0x260:
    case 0x380: case 0x384: case 0x388: case 0x38c:
        return;
    }
    qemu_log_mask(LOG_UNIMP, "edma: write 0x%04" HWADDR_PRIx " <- 0x%08x\n",
                  off, v);
}

static const MemoryRegionOps a8_edma_ops = {
    .read = a8_edma_read,
    .write = a8_edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void a8_edma_init(Object *obj)
{
    A8EdmaState *s = A8_EDMA(obj);

    memory_region_init_io(&s->iomem, obj, &a8_edma_ops, s, "edma-tpcc",
                          0x8000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bh = qemu_bh_new(a8_edma_run, s);
    for (int i = 0; i < NCH; i++) {
        s->dchmap[i] = i << 5;
    }
}

static const TypeInfo a8_edma_info = {
    .name = TYPE_A8_EDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(A8EdmaState),
    .instance_init = a8_edma_init,
};

static void a8_edma_register(void)
{
    type_register_static(&a8_edma_info);
}
type_init(a8_edma_register)
