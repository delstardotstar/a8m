#ifndef MMU_H_INCLUDED
#define MMU_H_INCLUDED

#include <stdint.h>

/* Top 1 MB of DDR, the linker's .dma section, mapped write-through at L1
 * and L2: CPU stores reach DDR at once, so buffers the EDMA
 * reads need no clean, but a buffer the EDMA writes must be invalidated
 * (dcache_inval_lines) before each read of what it wrote since. */
#define DMA_REGION 0x9ff00000u
/* Debug builds: unmapped 1 MB sections at both ends of the sample pool. */
#ifdef A8_DEBUG
#define POOL_GUARD 0x100000u
#else
#define POOL_GUARD 0u
#endif

/* Flat map, caches and branch prediction on. Call early, once. */
void mmu_init(void);

/* Data cache maintenance to the point of coherency (L1 and L2) for a range
 * of cacheable DDR, for DMA into it: clean+invalidate before the transfer
 * starts, invalidate again after it ends. Both cover whole 64-byte lines,
 * so the range should be line-aligned. */
#define CACHE_LINE 64u
void dcache_flush_range(const void *p, uint32_t n);
void dcache_inval_range(const void *p, uint32_t n);
/* Write-through (DMA_REGION) lines only (an invalidate drops nothing
 * dirty there): invalidate the lines over [p, p + n). No dsb: one
 * dcache_sync after a batch, before the first read or preload. */
static inline void dcache_inval_lines(const volatile void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~(CACHE_LINE - 1u), end = (uint32_t)p + n;

    for (; a < end; a += CACHE_LINE) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" :: "r"(a) : "memory");   /* DCIMVAC */
    }
}
static inline void dcache_sync(void)
{
    __asm__ volatile("dsb" ::: "memory");
}
/* Starts the line fills over [p, p + n) so they overlap. */
static inline void preload_lines(const volatile void *p, uint32_t n)
{
    uint32_t a = (uint32_t)p & ~(CACHE_LINE - 1u), end = (uint32_t)p + n;

    for (; a < end; a += CACHE_LINE) {
        __builtin_prefetch((const void *)a);
    }
}

#endif
