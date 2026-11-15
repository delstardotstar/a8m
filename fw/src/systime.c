#include "systime.h"
#include "hw.h"
#include "diag.h"

/* DMTimer2 on the 24 MHz master oscillator, free-running from 0. */
#define DMTIMER2        0x48040000u
#define TIMER_TCLR      0x038u
#define TIMER_TCRR      0x03cu
#define TIMER_TLDR      0x040u
#define TCLR_START_AUTORELOAD 0x3u
#define CM_PER_TIMER2   0x080u
#define CM_DPLL         0x44e00500u
#define CLKSEL_TIMER2   0x008u
#define CLKSEL_M_OSC    0x1u
#define TICKS_PER_US    24u

/* Test hooks (EXTRA=-D...): TIME_START presets the clock in us (the 32-bit
 * wrap at 4294967296); TIME_SKIP jumps it forward by that many us once, at
 * TIME_SKIP_AT us after boot, as if the module had sat idle. */
#ifndef TIME_START
#define TIME_START 0
#endif

static uint32_t last_ticks, frac;
static uint64_t us = TIME_START;

void time_init(void)
{
    REG(CM_DPLL + CLKSEL_TIMER2) = CLKSEL_M_OSC;
    clock_require(CM_PER, CM_PER_TIMER2, CLK_ENABLE);
    REG(DMTIMER2 + TIMER_TCLR) = 0;
    REG(DMTIMER2 + TIMER_TLDR) = 0;
    REG(DMTIMER2 + TIMER_TCRR) = 0;
    REG(DMTIMER2 + TIMER_TCLR) = TCLR_START_AUTORELOAD;
    last_ticks = 0;
}

/* Must be called at least every ~178 s (one counter wrap). IRQs are masked
 * around the update: a caller in an interrupt (the TEST_ hooks) must not
 * tear the accumulator. */
uint64_t time_us64(void)
{
    uint32_t f, ticks;
    uint64_t r;

    __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(f) :: "memory");
    ticks = REG(DMTIMER2 + TIMER_TCRR);
    frac += ticks - last_ticks;
    last_ticks = ticks;
    us += frac / TICKS_PER_US;
    frac %= TICKS_PER_US;
#ifdef TIME_SKIP
    static int skipped;
    if (!skipped && us - TIME_START >= (uint64_t)TIME_SKIP_AT) {
        skipped = 1;
        us += (uint64_t)TIME_SKIP;
    }
#endif
    r = us;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(f) : "memory");
    return r;
}

uint32_t time_us(void)
{
    return (uint32_t)time_us64();
}

uint32_t time_ticks(void)
{
    return REG(DMTIMER2 + TIMER_TCRR);
}
