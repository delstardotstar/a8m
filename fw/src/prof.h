#ifndef PROF_H_INCLUDED
#define PROF_H_INCLUDED

#include <stdint.h>

/* Audio ISR time per stage, from the Cortex-A8 cycle counter (PMU CCNT,
 * counting every 64th cycle). The ISR switches the running stage with
 * prof_to; whatever isn't in a named stage lands in PS_REST. */
enum {
    PS_SMP,         /* sampler voices */
    PS_MAC,         /* machine voices */
    PS_GRN,         /* grains */
    PS_TL,          /* Time Lock */
    PS_CFX,         /* channel EQ, filter, compressor */
    PS_MIX,         /* pan/mix, sends, output conversion */
    PS_RVB,         /* reverb (DEP-5, 70 or 80) */
    PS_DLY,         /* delay */
    PS_MCP,         /* mix compressor */
    PS_REST,        /* the rest of the ISR not in a PS_R* stage */
    PS_RGT,         /* gate edges, encoder, triggers */
    PS_RCV,         /* CV (cv_poll) */
    PS_RIN,         /* sample input from McASP1's ring */
    PS_ROUT,        /* output copies into McASP0's DMA buffer */
    PS_RM1,         /* mix copy into McASP1's DMA buffer */
    PS_COUNT
};
/* Audio Health shows the stages before PS_RGT, PS_REST as the sum of
 * PS_REST..PS_RM1 ("RST"). */
#define PS_SHOWN PS_RGT

extern const char *const prof_names[PS_COUNT];

struct prof_lap {
    uint32_t last, cur;
    uint32_t acc[PS_COUNT];
};
extern struct prof_lap prof;

static inline uint32_t prof_ccnt(void)
{
    uint32_t v;

    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
}

/* Audio ISR: charge the time since the last switch to the running stage
 * and run s from now; returns the stage that was running. */
static inline unsigned prof_to(unsigned s)
{
    uint32_t t = prof_ccnt();
    unsigned p = prof.cur;

    prof.acc[p] += t - prof.last;
    prof.last = t;
    prof.cur = s;
    return p;
}

/* Boot: enables the cycle counter. */
void prof_init(void);
/* Audio ISR entry (starts PS_REST without charging the time outside). */
static inline void prof_isr_enter(void)
{
    prof.last = prof_ccnt();
    prof.cur = PS_REST;
}
/* Audio ISR exit. */
static inline void prof_isr_exit(void)
{
    prof_to(PS_REST);
}
/* Audio ISR, once a block after the render: closes a 32-frame span (667
 * us) every 32 / AUDIO_BLOCK blocks; the figures are per span. */
void prof_block(void);
/* Main loop: re-measures the counter's rate against the 24 MHz timer. */
void prof_poll(void);

/* Per stage over the last published window (~2.7 s), us: worst span and
 * mean; peak is the worst span since boot. Index PS_COUNT is the total,
 * PS_RST_ALL the sum of PS_REST..PS_RM1. */
#define PS_ALL      PS_COUNT
#define PS_RST_ALL  (PS_COUNT + 1)
#define PS_VIEW     (PS_COUNT + 2)
struct prof_view {
    uint32_t max_us[PS_VIEW], peak_us[PS_VIEW];
    uint32_t mean_us10[PS_VIEW];            /* tenths of a us */
    uint32_t windows;                       /* published so far */
};
void prof_get(struct prof_view *v);

#endif
