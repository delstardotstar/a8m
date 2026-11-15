/*
 * MMU and caches. MLO enters us with the MMU off, which on the Cortex-A8
 * means no data caching: everything ran several times too slowly (hardware
 * run 12: the audio interrupt left the main loop almost no time).
 *
 * Flat 1 MB section map: DDR cacheable (write-back, write-allocate) except
 * its top 1 MB, the DMA buffers (DMA_REGION in mmu.h): write-through at
 * both levels; everything below DDR (GPMC, on-chip RAM, peripherals) is
 * device memory. The A8's L2 follows the outer attributes: an outer
 * write-back DMA buffer (TEX 1x1) keeps the CPU's stores from the EDMA
 * (f217f22: all audio garbled).
 */
#include "mmu.h"

#include <stdint.h>

#define SECTION         0x2u
#define AP_FULL         (3u << 10)
#define XN              (1u << 4)
#define DEVICE          (1u << 2)                           /* TEX 000, C 0, B 1 */
#define NORMAL_WB_WA    ((1u << 12) | (1u << 3) | (1u << 2)) /* TEX 001, C 1, B 1 */
#define NORMAL_WT       (1u << 3)                           /* TEX 000, C 1, B 0 */

static uint32_t table[4096] __attribute__((aligned(16384)));

#ifdef A8_DEBUG
/* Debug builds: stray accesses fault at once. Unmapped: the first and last
 * 1 MB (null pointers, small negative offsets), beyond DDR, and a section
 * at each end of the sample pool; code and constants are read-only (4 KB
 * pages over the image's first megabytes). */
#define L2_TABLES       4
#define PAGE_NORMAL_WB_WA ((1u << 6) | (1u << 3) | (1u << 2) | 0x2u)
#define PAGE_RW         (3u << 4)
#define PAGE_RO         ((1u << 9) | (1u << 4))
extern uint8_t __sample_pool[], __text_end[];
static uint32_t l2[L2_TABLES][256] __attribute__((aligned(1024)));

static void debug_map(void)
{
    uint32_t pool = (uint32_t)__sample_pool, text_end = (uint32_t)__text_end & ~0xfffu;

    table[0] = 0;
    table[4095] = 0;
    for (uint32_t i = 0xa00; i < 4095; i++) {
        table[i] = 0;
    }
    table[pool >> 20] = 0;
    table[(DMA_REGION >> 20) - 1] = 0;
    for (uint32_t t = 0; t < L2_TABLES && 0x80000000u + (t << 20) < text_end; t++) {
        uint32_t mb = 0x80000000u + (t << 20);
        for (uint32_t p = 0; p < 256; p++) {
            uint32_t a = mb + (p << 12);
            l2[t][p] = a | PAGE_NORMAL_WB_WA | (a < text_end ? PAGE_RO : PAGE_RW);
        }
        table[mb >> 20] = (uint32_t)l2[t] | 0x1u;
    }
}
#endif

/* Invalidate (not clean) every data/unified cache level by set/way. */
static void dcache_invalidate_all(void)
{
    uint32_t clidr;

    __asm__ volatile("mrc p15, 1, %0, c0, c0, 1" : "=r"(clidr));
    for (uint32_t level = 0; level < 7; level++) {
        uint32_t type = (clidr >> (level * 3)) & 7;
        uint32_t ccsidr, sets, ways, line_shift, way_shift;

        if (type < 2) {
            continue;
        }
        __asm__ volatile("mcr p15, 2, %0, c0, c0, 0\n isb" :: "r"(level << 1));
        __asm__ volatile("mrc p15, 1, %0, c0, c0, 0" : "=r"(ccsidr));
        line_shift = (ccsidr & 7) + 4;
        ways = ((ccsidr >> 3) & 0x3ff) + 1;
        sets = ((ccsidr >> 13) & 0x7fff) + 1;
        way_shift = __builtin_clz(ways - 1);
        for (uint32_t w = 0; w < ways; w++) {
            for (uint32_t s = 0; s < sets; s++) {
                uint32_t sw = (level << 1) | (s << line_shift) | (ways > 1 ? w << way_shift : 0);
                __asm__ volatile("mcr p15, 0, %0, c7, c6, 2" :: "r"(sw));   /* DCISW */
            }
        }
    }
    __asm__ volatile("dsb\n isb" ::: "memory");
}

void mmu_init(void)
{
    uint32_t sctlr;

    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t base = i << 20;

        if (base >= 0x80000000u && base < DMA_REGION) {
            table[i] = base | AP_FULL | NORMAL_WB_WA | SECTION;
        } else if (base >= 0xa0000000u) {
            /* Past the 512 MB of DDR: the controller would alias it onto
             * our code, so a stray access must fault instead. */
            table[i] = 0;
        } else if (base >= 0x80000000u) {
            table[i] = base | AP_FULL | NORMAL_WT | SECTION;
        } else {
            table[i] = base | AP_FULL | DEVICE | XN | SECTION;
        }
    }

#ifdef A8_DEBUG
    debug_map();
#endif
    dcache_invalidate_all();
    __asm__ volatile(
        "mov r0, #0\n"
        "mcr p15, 0, r0, c7, c5, 0\n"       /* ICIALLU */
        "mcr p15, 0, r0, c7, c5, 6\n"       /* BPIALL */
        "mcr p15, 0, r0, c8, c7, 0\n"       /* TLBIALL */
        "mcr p15, 0, r0, c2, c0, 2\n"       /* TTBCR: TTBR0 only */
        "mcr p15, 0, %0, c2, c0, 0\n"       /* TTBR0 */
        "mov r0, #1\n"
        "mcr p15, 0, r0, c3, c0, 0\n"       /* DACR: domain 0 client */
        "dsb\n isb\n"
        :: "r"((uint32_t)table) : "r0", "memory");

    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    sctlr |= (1u << 0) | (1u << 2) | (1u << 11) | (1u << 12);  /* M, C, Z, I */
    __asm__ volatile("mcr p15, 0, %0, c1, c0, 0\n dsb\n isb" :: "r"(sctlr) : "memory");
}

void dcache_flush_range(const void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~(CACHE_LINE - 1u), end = (uint32_t)p + n;

    for (; a < end; a += CACHE_LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(a) : "memory");  /* DCCIMVAC */
    }
    __asm__ volatile("dsb" ::: "memory");
}

void dcache_inval_range(const void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~(CACHE_LINE - 1u), end = (uint32_t)p + n;

    for (; a < end; a += CACHE_LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");   /* DCIMVAC */
    }
    __asm__ volatile("dsb" ::: "memory");
}
