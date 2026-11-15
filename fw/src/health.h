#ifndef HEALTH_H_INCLUDED
#define HEALTH_H_INCLUDED

#include <stdint.h>

/* The health log: one JSON line per entry appended to 1:A8M-HEALTH.JSONL
 * (Audio Health's figures, stage times, the preset's voices and effects).
 * Written from the main loop only; a line due while a storage job runs is
 * skipped and counted. */
#define HEALTH_PATH "1:A8M-HEALTH.JSONL"

/* UI: log a line ("manual"). */
void health_request(void);
/* After a WDT1 reset: logs a line ("watchdog", with the hang note). */
void health_watchdog(void);
/* Main loop: writes requested lines, and one when an overload burst
 * starts (at most one every 5 s, "overload"). */
void health_poll(void);
/* Lines written since boot. */
uint32_t health_lines(void);

#endif
