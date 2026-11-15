#include "prof.h"
#include "audio.h"
#include "systime.h"

#define WINDOW      4096u       /* 32-frame spans a published figure covers, ~2.7 s */
#define CAL_TICKS   (24000000u / 4u)
#define N           PS_VIEW

const char *const prof_names[PS_COUNT] = { "SMP", "MAC", "GRN", "TL", "CFX", "MIX", "RVB", "DLY", "MCP", "R-ETC",
                                           "R-GT", "R-CV", "R-IN", "R-OUT", "R-M1" };

struct prof_lap prof;
static uint32_t wmax[N], peak[N], wn;
static uint64_t wsum[N];
static struct {
    uint32_t max[N];
    uint64_t sum[N];
    uint32_t n, windows;
} pub;
/* Counter units (64 cycles) per us; the A8's MPU runs at ~998 MHz. */
static float units_per_us = 998.0f / 64.0f;

void prof_init(void)
{
    uint32_t pmcr;

    __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(pmcr));
    pmcr |= 1u | 4u | 8u;       /* enable, reset CCNT, count every 64th cycle */
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(pmcr));
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 3" :: "r"(1u << 31));
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(1u << 31));
    prof.last = prof_ccnt();
    prof.cur = PS_REST;
}

static void take(unsigned i, uint32_t v)
{
    wsum[i] += v;
    if (v > wmax[i]) {
        wmax[i] = v;
    }
    if (v > peak[i]) {
        peak[i] = v;
    }
}

void prof_block(void)
{
    static unsigned sub;
    uint32_t total = 0, rest = 0;

    prof_to(prof.cur);
    if (++sub < BLOCKS_PER_32) {
        return;
    }
    sub = 0;
    for (unsigned s = 0; s < PS_COUNT; s++) {
        uint32_t v = prof.acc[s];
        prof.acc[s] = 0;
        total += v;
        rest += s >= PS_REST ? v : 0;
        take(s, v);
    }
    take(PS_ALL, total);
    take(PS_RST_ALL, rest);
    if (++wn >= WINDOW) {
        for (unsigned i = 0; i < N; i++) {
            pub.max[i] = wmax[i];
            pub.sum[i] = wsum[i];
            wmax[i] = 0;
            wsum[i] = 0;
        }
        pub.n = wn;
        pub.windows++;
        wn = 0;
    }
}

void prof_poll(void)
{
    static uint32_t t0, c0;
    static int primed;
    uint32_t t = time_ticks(), c = prof_ccnt(), dt = t - t0;

    if (!primed || dt > 8u * CAL_TICKS) {
        primed = 1;
    } else if (dt < CAL_TICKS) {
        return;
    } else {
        units_per_us = (float)(c - c0) * 24.0f / (float)dt;
    }
    t0 = t;
    c0 = c;
}

void prof_get(struct prof_view *v)
{
    uint32_t f, max[N], pk[N], n;
    uint64_t sum[N];
    float k = units_per_us > 0.01f ? 1.0f / units_per_us : 0.0f;

    __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(f) :: "memory");
    for (unsigned i = 0; i < N; i++) {
        max[i] = pub.max[i];
        sum[i] = pub.sum[i];
        pk[i] = peak[i];
    }
    n = pub.n;
    v->windows = pub.windows;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(f) : "memory");
    for (unsigned i = 0; i < N; i++) {
        v->max_us[i] = (uint32_t)((float)max[i] * k + 0.5f);
        v->peak_us[i] = (uint32_t)((float)pk[i] * k + 0.5f);
        v->mean_us10[i] = n ? (uint32_t)((float)sum[i] / (float)n * k * 10.0f + 0.5f) : 0;
    }
}
