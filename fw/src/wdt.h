#ifndef WDT_H_INCLUDED
#define WDT_H_INCLUDED

#include <stdint.h>

/* WDT1, the SoC's hardware watchdog: resets the module when nothing kicks
 * it for WDT_TIMEOUT_S. A kick counts only when the audio IRQ has run since
 * the last one, so a stuck main loop or a dead audio IRQ both lead to a
 * reset. MLO leaves it stopped; -DNO_WDT keeps it so. */
#define WDT_TIMEOUT_S 6u

/* Boot, first thing: reads and clears PRM_RSTST, takes the hang note. */
void wdt_boot(void);
/* Main loop entry: starts WDT1. */
void wdt_start(void);
/* Main loop, and inside long synchronous work (card I/O). Cheap. */
void wdt_kick(void);
/* Crash reports: stops WDT1 so the report stays up. */
void wdt_stop(void);
int wdt_running(void);

/* PRM_RSTST as found at boot; WDT1_RST = the last reset was the watchdog. */
#define RSTST_WDT1 0x10u
extern uint32_t wdt_reset_cause;
static inline int wdt_recovered(void)
{
    return (wdt_reset_cause & RSTST_WDT1) != 0;
}

/* What the main loop was doing when it stalled (diag.c records it once the
 * loop has been still for a while; kept in RAM across the reset). */
struct wdt_hang {
    uint32_t pc, ip, lr, sp, main_tag, irq_tag, blocks, uptime_ms;
};
void wdt_note_hang(const struct wdt_hang *h);
void wdt_clear_hang(void);
/* The note from before the reset, or 0. */
const struct wdt_hang *wdt_last_hang(void);

#endif
