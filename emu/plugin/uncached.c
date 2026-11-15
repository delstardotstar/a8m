/*
 * TCG plugin: NEON/VFP loads (VLD1-4, VLDR, VLDM) from Normal non-cacheable
 * memory hang the real Cortex-A8; flag them and stop the emulator.
 * The memory type comes from the guest's short-descriptor translation
 * tables, walked once per 4 KB page and then cached: tables are assumed not
 * to change once the MMU is on. TEX remap (SCTLR.TRE) is not handled.
 * Args: vfp=off (only NEON structure loads), halt=off (log each PC once and
 * go on). A32 only: the plugin can't see the Thumb bit; our firmware is -marm.
 *
 * Also: QEMU has no caches, so turning the D-cache off at runtime (SCTLR.C
 * 1 -> 0) can't go wrong here, but on the A8 a store made with it off goes
 * to DDR while a stale dirty line for the same address may still sit in the
 * cache, and a later clean writes that line over it. Once C goes off, every
 * DDR store is watched; a clean by set/way after one, or by MVA of its line,
 * halts. (Hardware: Load Software's stack was overwritten this way.)
 *
 * Also (dma=off turns it off): which DDR lines hold CPU stores that have
 * not reached DDR yet. The A8 sends a store to DDR at once only when both
 * cache levels are write-through or non-cacheable for it: L1 follows the
 * inner attributes, the L2 the outer ones unless L2ACTLR bit 16 ("L2
 * inner") is set (A8 TRM, Table 7-1 and c9 L2 Cache Auxiliary Control).
 * Inner write-through over outer write-back (TEX 1x1 C 1 B 0) leaves the
 * store dirty in the L2. A line is clean again after a clean to PoC by
 * MVA, any clean or invalidate by set/way. The EDMA model hands every DDR
 * range it reads or writes to on_dma (through A8_DMA_HOOK): a read of a
 * dirty line (the device gets stale DDR) or a write under one (an
 * eviction later puts the CPU's copy back) halts, and so does an
 * invalidate by MVA of a dirty line (the CPU's store is lost). Lines are
 * not tracked otherwise (no fills or evictions: a dirty line the A8 would
 * have evicted in time is still flagged). l2inner=on starts with L2ACTLR
 * bit 16 set; map=on prints the DDR/OCMC mapping when the MMU goes on.
 * Page types come from walking the tables then and after each TLB
 * invalidate.
 *
 * And (stale=off turns it off) the other direction, for inner cacheable
 * pages in the top 1 MB of DDR (WT_BASE: our .dma and stock's CV ring):
 * 64-byte lines, a CPU load fills one (a copy of DDR then), stores update
 * it, an invalidate (by MVA, or any by set/way) drops it. A load whose
 * bytes in the copy differ from DDR read stale data on the A8 (the EDMA
 * wrote DDR after the fill with no invalidate between): it halts. Real
 * lines also get evicted, so this only flags what the A8 may get wrong.
 * Changes within RACE memory accesses of the fill are the EDMA writing
 * while the CPU reads (cv.c copies whole 32-byte pieces past the newest
 * scan; the emulator's ADCs also catch up in bursts): not flagged. A
 * missed invalidate shows a block later, far past RACE. Stock maps all of
 * DDR inner write-through and words elsewhere change with no CPU store or
 * DMA this model sees, so only the top 1 MB is modelled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

enum { UNKNOWN, CACHED, UNCACHED };

static bool vfp = true, halt = true, dma = true, stale = true, show_map;
static uint8_t page_type[1u << 20];
static GHashTable *seen;
static GMutex lock;
static struct qemu_plugin_register *r_sctlr[2], *r_ttbr0[2], *r_ttbr1[2], *r_ttbcr[2];
static struct qemu_plugin_register *r_gpr[15];
static qemu_plugin_id_t self;

enum { SCTLR, CLEAN_SW, CLEAN_MVA, INV_SW, INV_MVA, TLB, L2ACTLR, ACTLR };
typedef struct {
    uint32_t pc;
    uint8_t op, rt, pou, inv;
} Mcr;
static bool cache_was_on, watching, c_off, stored;
static uint32_t store_pc, store_pa;

static const char *kind(uint32_t w)
{
    if ((w & 0xff300000) == 0xf4200000) {
        return "NEON VLDn";
    }
    if (vfp && (w >> 28) != 0xf && (w & 0x0e100e00) == 0x0c100a00 &&
        (w & 0x01a00000)) {
        return (w & 0x01200000) == 0x01000000 ? "VFP VLDR" : "VFP VLDM";
    }
    return NULL;
}

static uint64_t reg(struct qemu_plugin_register *r)
{
    g_autoptr(GByteArray) b = g_byte_array_new();
    uint64_t v = 0;

    if (r && qemu_plugin_read_register(r, b)) {
        memcpy(&v, b->data, MIN(b->len, sizeof(v)));
    }
    return v;
}

static int mcr_op(uint32_t w)
{
    unsigned crn = (w >> 16) & 15, opc1 = (w >> 21) & 7, crm = w & 15, opc2 = (w >> 5) & 7;

    if ((w >> 28) == 0xf || (w & 0x0f100f10) != 0x0e000f10 || opc1 || ((w >> 12) & 15) == 15) {
        return -1;
    }
    if (crn == 1 && crm == 0 && opc2 == 0) {
        return SCTLR;
    }
    if (crn == 7 && (crm == 10 || crm == 14) && opc2 == 2) {
        return CLEAN_SW;
    }
    if (crn == 7 && (crm == 10 || crm == 11 || crm == 14) && opc2 == 1) {
        return CLEAN_MVA;
    }
    if (crn == 7 && crm == 6 && opc2 == 2) {
        return INV_SW;
    }
    if (crn == 7 && crm == 6 && opc2 == 1) {
        return INV_MVA;
    }
    if (crn == 8) {
        return TLB;
    }
    if (crn == 1 && crm == 0 && opc2 == 1) {
        return ACTLR;
    }
    return -1;
}

/* MCR p15, 1, Rt, c9, c0, 2 */
static bool is_l2actlr(uint32_t w)
{
    return (w >> 28) != 0xf && (w & 0x0ff00fff) == 0x0e290f50 && ((w >> 12) & 15) != 15;
}

static bool phys32(uint64_t pa, uint32_t *v)
{
    g_autoptr(GByteArray) b = g_byte_array_new();

    if (qemu_plugin_read_memory_hwaddr(pa, b, 4) != QEMU_PLUGIN_HWADDR_OPERATION_OK) {
        return false;
    }
    memcpy(v, b->data, 4);
    return true;
}

/* Memory type of va from the live tables; *desc gets the final descriptor. */
static int walk(uint32_t va, uint32_t *desc)
{
    int s = (reg(r_sctlr[1]) & 1) ? 1 : 0;
    uint32_t sctlr = reg(r_sctlr[s]), ttbcr = reg(r_ttbcr[s]);
    unsigned n = ttbcr & 7;
    uint32_t base, d, tex, cb;

    if (!(sctlr & 1) || (sctlr & (1u << 28)) || (ttbcr & (1u << 31))) {
        return UNKNOWN;
    }
    if (n == 0 || (va >> (32 - n)) == 0) {
        base = reg(r_ttbr0[s]) & ~((1u << (14 - n)) - 1);
    } else {
        base = reg(r_ttbr1[s]) & ~0x3fffu;
    }
    if (!phys32(base + (va >> 20) * 4, &d)) {
        return UNKNOWN;
    }
    if ((d & 3) == 2) {
        tex = (d >> 12) & 7;
        cb = (d >> 2) & 3;
    } else if ((d & 3) == 1) {
        if (!phys32((d & ~0x3ffu) + ((va >> 12) & 0xff) * 4, &d)) {
            return UNKNOWN;
        }
        if ((d & 3) == 1) {
            tex = (d >> 12) & 7;
        } else if (d & 2) {
            tex = (d >> 6) & 7;
        } else {
            return UNKNOWN;
        }
        cb = (d >> 2) & 3;
    } else {
        return UNKNOWN;
    }
    *desc = d;
    /* Normal non-cacheable: TEX 001 CB 00, or TEX 1xx with inner policy 00. */
    if ((tex == 1 && cb == 0) || (tex >= 4 && cb == 0)) {
        return UNCACHED;
    }
    return CACHED;
}


/* Cache policy at one level (short descriptors, TEX remap off). */
enum { P_NONE, P_NC, P_WT, P_WB };
#define DDR_BASE 0x80000000u
#define DDR_SIZE 0x20000000u

static uint8_t pol[1u << 20];           /* inner | outer << 4; 0xff unmapped */
static uint32_t page_pa[1u << 20];      /* frame number */
static uint8_t dirty[DDR_SIZE / 64 / 8];
static uint32_t dirty_pc[DDR_SIZE / 64];
static unsigned n_dirty;
static bool mmu_on, l2inner, l2en = true, tre_warned;

static unsigned area(unsigned cb)
{
    static const uint8_t p[4] = { P_NC, P_WB, P_WT, P_WB };

    return p[cb];
}

static uint8_t attrs(uint32_t tex, uint32_t cb)
{
    if (tex & 4) {
        return area(cb) | area(tex & 3) << 4;
    }
    if (tex == 0 && cb >= 2) {
        return (cb == 2 ? P_WT : P_WB) * 0x11;
    }
    if (tex == 1 && cb == 0) {
        return P_NC * 0x11;
    }
    if (tex == 1 && cb == 3) {
        return P_WB * 0x11;
    }
    return P_NONE;
}

static const char *pname(unsigned p)
{
    static const char *n[] = { "device", "NC", "WT", "WB" };

    return n[p & 3];
}

static void print_map(uint32_t sctlr)
{
    uint32_t start = 0;

    for (uint32_t i = 1; i <= (1u << 20); i++) {
        if (i < (1u << 20) && pol[i] == pol[start] && page_pa[i] == page_pa[start] + (i - start)) {
            continue;
        }
        if (pol[start] != 0xff && ((page_pa[start] >= 0x80000 && page_pa[start] < 0xa0000) ||
                                   (page_pa[start] >= 0x40300 && page_pa[start] < 0x40310))) {
            fprintf(stderr, "a8 map: va 0x%08x-0x%08x pa 0x%08x inner %s outer %s\n",
                    start << 12, (i << 12) - 1, page_pa[start] << 12,
                    pname(pol[start]), pname(pol[start] >> 4));
        }
        start = i;
    }
    fprintf(stderr, "a8 map: SCTLR 0x%08x L2ACTLR bit 16 %d\n", sctlr, l2inner);
}

static void watch_lines(void);

static void classify(uint32_t sctlr)
{
    int s = r_sctlr[1] != r_sctlr[0];
    uint32_t ttbcr = reg(r_ttbcr[s]), n = ttbcr & 7, base, d, e;

    memset(pol, 0xff, sizeof(pol));
    if (!(sctlr & 1) || (ttbcr & (1u << 31))) {
        return;
    }
    if ((sctlr & (1u << 28)) && !tre_warned) {
        tre_warned = true;
        fprintf(stderr, "uncached plugin: SCTLR.TRE set; TEX remap not modelled\n");
    }
    for (uint32_t i = 0; i < 4096; i++) {
        if (n == 0 || (i >> (12 - n)) == 0) {
            base = reg(r_ttbr0[s]) & ~((1u << (14 - n)) - 1);
        } else {
            base = reg(r_ttbr1[s]) & ~0x3fffu;
        }
        if (!phys32(base + i * 4, &d)) {
            continue;
        }
        if ((d & 3) == 2) {
            bool super = d & (1u << 18);
            uint32_t f = ((super ? d & 0xff000000u : d & 0xfff00000u) >> 12) + (super ? (i & 15) << 8 : 0);
            uint8_t a = attrs((d >> 12) & 7, (d >> 2) & 3);

            for (uint32_t j = 0; j < 256; j++) {
                pol[(i << 8) | j] = a;
                page_pa[(i << 8) | j] = f + j;
            }
        } else if ((d & 3) == 1) {
            for (uint32_t j = 0; j < 256; j++) {
                if (!phys32((d & ~0x3ffu) + j * 4, &e) || !(e & 3)) {
                    continue;
                }
                if ((e & 3) == 1) {
                    pol[(i << 8) | j] = attrs((e >> 12) & 7, (e >> 2) & 3);
                    page_pa[(i << 8) | j] = ((e >> 12) & ~15u) + (j & 15);
                } else {
                    pol[(i << 8) | j] = attrs((e >> 6) & 7, (e >> 2) & 3);
                    page_pa[(i << 8) | j] = e >> 12;
                }
            }
        }
    }
    if (show_map) {
        print_map(sctlr);
    }
    watch_lines();
}

#define LINE 64u
#define RACE 8192u
#define WT_BASE 0x9ff00000u
typedef struct {
    uint64_t filled;
    uint8_t b[LINE];
} Line;
static GHashTable *wt_lines;
static uint64_t accesses;
static bool wt_watch;

static void on_reset(void *udata);

static bool wt_modelled(uint32_t va)
{
    uint8_t a = pol[va >> 12];

    return va >= WT_BASE && a != 0xff && ((a & 15) == P_WT || (a & 15) == P_WB);
}

static void watch_lines(void)
{
    for (uint32_t va = WT_BASE; stale && !wt_watch && va >= WT_BASE; va += 4096) {
        if (wt_modelled(va)) {
            wt_watch = true;
            qemu_plugin_reset(self, on_reset, NULL);
        }
    }
}

static GByteArray *phys_read(uint64_t pa, unsigned n)
{
    GByteArray *b = g_byte_array_new();

    if (qemu_plugin_read_memory_hwaddr(pa, b, n) != QEMU_PLUGIN_HWADDR_OPERATION_OK || b->len < n) {
        g_byte_array_unref(b);
        return NULL;
    }
    return b;
}

static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *udata)
{
    uint32_t va = vaddr, off = va & (LINE - 1), n = 1u << qemu_plugin_mem_size_shift(info);
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa;
    Line *line;

    accesses++;
    if (!mmu_on || !wt_modelled(va)) {
        return;
    }
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw || qemu_plugin_hwaddr_is_io(hw)) {
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    n = MIN(n, LINE - off);
    g_mutex_lock(&lock);
    line = g_hash_table_lookup(wt_lines, GUINT_TO_POINTER(va / LINE));
    if (qemu_plugin_mem_is_store(info)) {
        g_autoptr(GByteArray) b = line ? phys_read(pa, n) : NULL;
        if (b) {
            memcpy(line->b + off, b->data, n);
        }
    } else if (!line) {
        g_autoptr(GByteArray) b = phys_read(pa - off, LINE);
        if (b) {
            line = g_new(Line, 1);
            line->filled = accesses;
            memcpy(line->b, b->data, LINE);
            g_hash_table_insert(wt_lines, GUINT_TO_POINTER(va / LINE), line);
        }
    } else {
        g_autoptr(GByteArray) b = phys_read(pa, n);
        if (b && memcmp(line->b + off, b->data, n) && accesses - line->filled < RACE) {
            memcpy(line->b + off, b->data, n);
        } else if (b && memcmp(line->b + off, b->data, n)) {
            g_mutex_unlock(&lock);
            fprintf(stderr, "a8: stale read from cached memory: PC 0x%08" PRIx64
                    " addr 0x%08x: DDR changed after the line was filled and no "
                    "invalidate came between%s\n", (uint64_t)(uintptr_t)udata, va,
                    halt ? " -- halting" : "");
            fflush(stderr);
            if (halt) {
                _exit(3);
            }
            g_mutex_lock(&lock);
            memcpy(line->b + off, b->data, n);
        }
    }
    g_mutex_unlock(&lock);
}

static void drop_lines(Mcr *m, uint32_t va)
{
    g_mutex_lock(&lock);
    if (m->op == INV_SW || m->op == CLEAN_SW) {
        g_hash_table_remove_all(wt_lines);
    } else {
        g_hash_table_remove(wt_lines, GUINT_TO_POINTER(va / LINE));
    }
    g_mutex_unlock(&lock);
}

/* A store to this page can stay in a cache instead of reaching DDR. */
static bool holds_dirty(uint8_t a)
{
    unsigned in = a & 15, out = l2inner ? in : a >> 4;

    return a != 0xff && (in == P_WB || (l2en && out == P_WB));
}

/* halt=off: each (line, kind, store PC) once. */
static void report(const char *what, int kind, uint32_t pa, uint32_t pc)
{
    static GHashTable *once;

    if (!halt) {
        gint64 *k = g_new(gint64, 1);
        bool fresh;

        *k = (gint64)pc << 32 | (pa & ~63u) | kind;
        g_mutex_lock(&lock);
        once = once ? once : g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
        fresh = g_hash_table_add(once, k);
        g_mutex_unlock(&lock);
        if (!fresh) {
            return;
        }
    }
    fprintf(stderr, "a8: %s: line 0x%08x (CPU store at PC 0x%08x not cleaned to DDR)%s\n",
            what, pa & ~63u, pc, halt ? " -- halting" : "");
    fflush(stderr);
    if (halt) {
        _exit(3);
    }
}

/* Under lock: clears pa's line, true (and its store's PC) if it was dirty. */
static bool take_line(uint32_t pa, uint32_t *pc)
{
    uint32_t l = (pa - DDR_BASE) / 64u;
    uint8_t m = 1u << (l & 7);

    if (pa < DDR_BASE || pa - DDR_BASE >= DDR_SIZE || !(dirty[l / 8] & m)) {
        return false;
    }
    dirty[l / 8] &= ~m;
    n_dirty--;
    *pc = dirty_pc[l];
    return true;
}

/* From the EDMA model (main loop thread). */
static void on_dma(int write, uint64_t pa, uint32_t n)
{
    uint32_t pc;

    g_mutex_lock(&lock);
    for (uint64_t a = pa & ~63ull; n_dirty && a < pa + n; a += 64) {
        if (take_line(a, &pc)) {
            g_mutex_unlock(&lock);
            report(write ? "EDMA wrote DDR under a dirty cache line (an eviction puts the "
                   "CPU's copy back)" : "EDMA read stale DDR (the newest data is only in "
                   "the cache)", write, a, pc);
            g_mutex_lock(&lock);
        }
    }
    g_mutex_unlock(&lock);
}

static void on_dma_store(unsigned int vcpu, qemu_plugin_meminfo_t info,
                         uint64_t vaddr, void *udata)
{
    uint32_t va = vaddr, n = 1u << qemu_plugin_mem_size_shift(info);

    if (!mmu_on) {
        return;
    }
    for (uint32_t a = va & ~63u; a < va + n; a += 64) {
        uint32_t pa = (page_pa[a >> 12] << 12) | (a & 0xfff), l = (pa - DDR_BASE) / 64u;

        if (!holds_dirty(pol[a >> 12]) || pa < DDR_BASE || pa - DDR_BASE >= DDR_SIZE ||
            (dirty[l / 8] & (1u << (l & 7)))) {
            continue;
        }
        g_mutex_lock(&lock);
        dirty[l / 8] |= 1u << (l & 7);
        n_dirty++;
        dirty_pc[l] = (uintptr_t)udata;
        g_mutex_unlock(&lock);
    }
}

static void dma_mcr(int op, uint32_t v, uint32_t pc)
{
    uint32_t pa = (page_pa[v >> 12] << 12) | (v & 0xfff), spc;
    bool lost = false;

    switch (op) {
    case SCTLR:
        if ((v & 1) && !mmu_on) {
            classify(v);
        }
        mmu_on = (v & 5) == 5;
        break;
    case TLB:
        if (mmu_on) {
            classify(reg(r_sctlr[(reg(r_sctlr[1]) & 1) ? 1 : 0]));
        }
        break;
    case L2ACTLR:
        l2inner = (v >> 16) & 1;
        break;
    case ACTLR:
        l2en = (v >> 1) & 1;
        break;
    case CLEAN_SW:
    case INV_SW:
        g_mutex_lock(&lock);
        if (n_dirty) {
            memset(dirty, 0, sizeof(dirty));
            n_dirty = 0;
        }
        g_mutex_unlock(&lock);
        break;
    case CLEAN_MVA:
    case INV_MVA:
        if (!mmu_on || pol[v >> 12] == 0xff) {
            break;
        }
        g_mutex_lock(&lock);
        lost = take_line(pa, &spc) && op == INV_MVA;
        g_mutex_unlock(&lock);
        if (lost) {
            g_autofree char *w = g_strdup_printf("invalidate by MVA at PC 0x%08x drops "
                                                 "a dirty line", pc);
            report(w, 2, pa, spc);
        }
        break;
    }
}

static void on_load(unsigned int vcpu, qemu_plugin_meminfo_t info,
                    uint64_t vaddr, void *udata)
{
    uint32_t va = vaddr, desc = 0;
    uint8_t t = page_type[va >> 12];
    uint64_t pc = (uintptr_t)udata;

    if (t == CACHED) {
        return;
    }
    if (t == UNKNOWN) {
        struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);

        if (hw && qemu_plugin_hwaddr_is_io(hw)) {
            return;
        }
        t = walk(va, &desc);
        if (t == UNKNOWN) {
            return;
        }
        page_type[va >> 12] = t;
        if (t == CACHED) {
            return;
        }
    }
    if (!halt) {
        g_mutex_lock(&lock);
        bool fresh = g_hash_table_add(seen, (gpointer)(uintptr_t)pc);
        g_mutex_unlock(&lock);
        if (!fresh) {
            return;
        }
    }
    fprintf(stderr, "a8: vector load from Normal non-cacheable memory hangs "
            "the A8: PC 0x%08" PRIx64 " addr 0x%08x%s\n",
            pc, va, halt ? " -- halting" : "");
    fflush(stderr);
    if (halt) {
        _exit(3);
    }
}

static void on_store(unsigned int vcpu, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw;
    uint64_t pa;

    if (!c_off || stored) {
        return;
    }
    hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw || qemu_plugin_hwaddr_is_io(hw)) {
        return;
    }
    pa = qemu_plugin_hwaddr_phys_addr(hw);
    if (pa >= 0x80000000u && pa < 0xa0000000u) {
        stored = true;
        store_pc = (uintptr_t)udata;
        store_pa = pa;
    }
}

static void on_mcr(unsigned int vcpu, void *udata)
{
    Mcr *m = udata;
    uint32_t v;

    /* DCCMVAU (c7, c11, 1) cleans to PoU only, not to DDR. */
    if ((dma || stale) && !m->pou) {
        dma_mcr(m->op, reg(r_gpr[m->rt]), m->pc);
    }
    if (m->inv && wt_watch) {
        drop_lines(m, reg(r_gpr[m->rt]));
    }
    if (m->op != SCTLR && m->op != CLEAN_SW && m->op != CLEAN_MVA) {
        return;
    }
    if (m->op != SCTLR && !stored) {
        return;
    }
    v = reg(r_gpr[m->rt]);
    if (m->op == SCTLR) {
        uint32_t cur = reg(r_sctlr[(reg(r_sctlr[1]) & 1) ? 1 : 0]);

        cache_was_on |= (cur & 4) != 0;
        c_off = !(v & 4);
        if (c_off && cache_was_on && !watching) {
            watching = true;
            qemu_plugin_reset(self, on_reset, NULL);
        }
        return;
    }
    if ((m->op == CLEAN_MVA && (v >> 6) != (store_pa >> 6))) {
        return;
    }
    fprintf(stderr, "a8: D-cache off at runtime: store at PC 0x%08x to 0x%08x went "
            "to DDR, then a clean (PC 0x%08x) can write a stale dirty line over "
            "it on the A8%s\n", store_pc, store_pa, m->pc, halt ? " -- halting" : "");
    fflush(stderr);
    if (halt) {
        _exit(3);
    }
    stored = false;
}

static void on_tb(struct qemu_plugin_tb *tb, void *udata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t pc = qemu_plugin_insn_vaddr(insn);
        uint32_t w;
        int op;

        if (qemu_plugin_insn_size(insn) != 4 ||
            qemu_plugin_insn_data(insn, &w, 4) != 4) {
            continue;
        }
        if (watching) {
            qemu_plugin_register_vcpu_mem_cb(insn, on_store, QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_W, (void *)(uintptr_t)pc);
        }
        if (wt_watch) {
            qemu_plugin_register_vcpu_mem_cb(insn, on_mem, QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_RW, (void *)(uintptr_t)pc);
        }
        if (dma) {
            qemu_plugin_register_vcpu_mem_cb(insn, on_dma_store, QEMU_PLUGIN_CB_NO_REGS,
                                             QEMU_PLUGIN_MEM_W, (void *)(uintptr_t)pc);
        }
        if ((op = is_l2actlr(w) ? L2ACTLR : mcr_op(w)) >= 0) {
            Mcr *m = g_new(Mcr, 1);

            m->pc = pc;
            m->op = op;
            m->rt = (w >> 12) & 15;
            m->pou = op == CLEAN_MVA && (w & 15) == 11;
            /* c7, c6 and c7, c14 drop the line too. */
            m->inv = (op == INV_SW || op == INV_MVA || op == CLEAN_SW || op == CLEAN_MVA) &&
                      ((w & 15) == 6 || (w & 15) == 14);
            qemu_plugin_register_vcpu_insn_exec_cb(insn, on_mcr, QEMU_PLUGIN_CB_R_REGS, m);
        }
        if (kind(w)) {
            qemu_plugin_register_vcpu_mem_cb(
                insn, on_load, QEMU_PLUGIN_CB_R_REGS, QEMU_PLUGIN_MEM_R,
                (void *)(uintptr_t)pc);
        }
    }
}

/* Retranslate with every store watched (QEMU flushes its TBs on a reset). */
static void on_reset(void *udata)
{
    qemu_plugin_register_vcpu_tb_trans_cb(self, on_tb, NULL);
}

/* [0] = Non-secure bank, [1] = Secure ("_S"); QEMU's A8 starts Secure. */
static void on_init(unsigned int vcpu, void *udata)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();

    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        bool sec = g_str_has_suffix(d->name, "_S");
        g_autofree char *base = g_strndup(d->name, strlen(d->name) - (sec ? 2 : 0));

        if (!sec && base[0] == 'r' && g_ascii_isdigit(base[1])) {
            int k = atoi(base + 1);
            if (k >= 0 && k < 13) {
                r_gpr[k] = d->handle;
            }
        } else if (!strcmp(base, "sp")) {
            r_gpr[13] = d->handle;
        } else if (!strcmp(base, "lr")) {
            r_gpr[14] = d->handle;
        } else if (!strcmp(base, "SCTLR_EL1")) {
            r_sctlr[sec] = d->handle;
        } else if (!strcmp(base, "TTBR0_EL1")) {
            r_ttbr0[sec] = d->handle;
        } else if (!strcmp(base, "TTBR1_EL1")) {
            r_ttbr1[sec] = d->handle;
        } else if (!strcmp(base, "TTBCR")) {
            r_ttbcr[sec] = d->handle;
        }
    }
    if (!r_sctlr[0] || !r_ttbr0[0] || !r_ttbcr[0]) {
        fprintf(stderr, "uncached plugin: no SCTLR/TTBR0/TTBCR registers\n");
        exit(1);
    }
    for (int s = 0; s < 2; s++) {
        if (!r_sctlr[s]) {
            r_sctlr[s] = r_sctlr[0];
            r_ttbr0[s] = r_ttbr0[0];
            r_ttbr1[s] = r_ttbr1[0];
            r_ttbcr[s] = r_ttbcr[0];
        }
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);
        bool ok = false;

        if (kv[1] && !strcmp(kv[0], "vfp")) {
            ok = qemu_plugin_bool_parse(kv[0], kv[1], &vfp);
        } else if (kv[1] && !strcmp(kv[0], "halt")) {
            ok = qemu_plugin_bool_parse(kv[0], kv[1], &halt);
        } else if (kv[1] && !strcmp(kv[0], "stale")) {
            ok = qemu_plugin_bool_parse(kv[0], kv[1], &stale);
        } else if (kv[1] && !strcmp(kv[0], "dma")) {
            ok = qemu_plugin_bool_parse(kv[0], kv[1], &dma);
        } else if (kv[1] && !strcmp(kv[0], "l2inner")) {
            ok = qemu_plugin_bool_parse(kv[0], kv[1], &l2inner);
        } else if (kv[1] && !strcmp(kv[0], "map")) {
            ok = qemu_plugin_bool_parse(kv[0], kv[1], &show_map);
        }
        if (!ok) {
            fprintf(stderr, "uncached plugin: bad option %s\n", argv[i]);
            return -1;
        }
    }
    self = id;
    seen = g_hash_table_new(NULL, NULL);
    memset(pol, 0xff, sizeof(pol));
    wt_lines = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    if (dma) {
        g_autofree char *h = g_strdup_printf("%" PRIxPTR, (uintptr_t)on_dma);

        g_setenv("A8_DMA_HOOK", h, TRUE);
    }
    qemu_plugin_register_vcpu_init_cb(id, on_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(self, on_tb, NULL);
    return 0;
}
