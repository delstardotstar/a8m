#ifndef UPDATE_H_INCLUDED
#define UPDATE_H_INCLUDED

#include <stdint.h>

/*
 * Load Software: installs 1:app (front card) as 0:app (boot card). Its
 * boot card writes (the only ones besides Calibrate's cal.bin, cal.h):
 * 0:app.new, then 0:app -> 0:app.bak and 0:app.new -> 0:app. Main loop
 * only; the work runs in update_poll.
 */
enum {
    UPD_IDLE, UPD_SCANNING, UPD_NONE, UPD_BAD, UPD_FOUND,
    UPD_COPYING, UPD_VERIFYING, UPD_FAILED, UPD_DONE
};
enum { APP_OURS, APP_STOCK, APP_UNKNOWN };

/* "A8M <version>", at a fixed place in the image (main.c). */
extern const char fw_version[32];

/* Back to UPD_IDLE (nothing found yet). */
void update_clear(void);
/* Reads and checks 1:app (UPD_SCANNING, then NONE / BAD / FOUND). */
void update_find(void);
/* From FOUND: starts the copy; 0 with update_error set if it can't. */
int update_install(void);
/* Stops a scan, copy or verify (the boot card is left as it was). */
void update_cancel(void);
void update_poll(void);
int update_state(void);
int update_busy(void);
int update_percent(void);
/* FOUND: what 1:app is ("A8M 5b483618", "STOCK 3.04", "UNKNOWN APP"). */
const char *update_found(void);
int update_kind(void);
uint32_t update_size(void);
/* Why NONE / BAD / FAILED, or 0. */
extern const char *update_error;
/* Mutes the outputs, writes pending prefs, then a global software reset;
 * if that doesn't happen, says POWER CYCLE and stops. */
void update_reset(void) __attribute__((noreturn));

#endif
