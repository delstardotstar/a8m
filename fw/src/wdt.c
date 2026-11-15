#include "wdt.h"
#include "audio.h"
#include "hw.h"
#include "systime.h"

#define WDT1            0x44e35000u
#define WDT_WCLR        0x24u
#define WDT_WCRR        0x28u
#define WDT_WLDR        0x2cu
#define WDT_WTGR        0x30u
#define WDT_WWPS        0x34u
#define WDT_WSPR        0x48u
#define WWPS_WCLR       0x01u
#define WWPS_WCRR       0x02u
#define WWPS_WLDR       0x04u
#define WWPS_WTGR       0x08u
#define WWPS_WSPR       0x10u
#define CM_WKUP_WDT1    0x0d4u
#define CLKSEL_WDT1     0x44e00538u     /* 1: the 32 768 Hz clock, not the RC oscillator */
#define PRM_RSTST       0x44e00f08u
#define WDT_HZ          32768u
#define KICK_TICKS      (24000000u / 20u)   /* kicks at most every 50 ms */
#define HANG_MAGIC      0x474e4148u         /* "HANG" */

uint32_t wdt_reset_cause;
static int running;
static uint32_t trigger, kick_at, kick_blocks;

/* Survives the watchdog's warm reset if MLO leaves that RAM alone (it
 * loads app below it); the check guards against anything else. */
static struct {
    uint32_t magic;
    struct wdt_hang h;
    uint32_t check;
} note __attribute__((section(".noinit")));
static struct wdt_hang last;
static int have_last;

static uint32_t note_check(void)
{
    const uint32_t *w = (const uint32_t *)&note.h;
    uint32_t c = HANG_MAGIC;

    for (unsigned i = 0; i < sizeof(note.h) / 4u; i++) {
        c = (c << 5 | c >> 27) ^ w[i];
    }
    return c;
}

static void wait_posted(uint32_t bit)
{
    uint32_t t0 = time_ticks();

    while ((REG(WDT1 + WDT_WWPS) & bit) && time_ticks() - t0 < 2400000u) {
    }
}

static void write_spr(uint32_t a, uint32_t b)
{
    wait_posted(WWPS_WSPR);
    REG(WDT1 + WDT_WSPR) = a;
    wait_posted(WWPS_WSPR);
    REG(WDT1 + WDT_WSPR) = b;
    wait_posted(WWPS_WSPR);
}

void wdt_boot(void)
{
    wdt_reset_cause = REG(PRM_RSTST);
    REG(PRM_RSTST) = wdt_reset_cause;
    if (note.magic == HANG_MAGIC && note.check == note_check() && wdt_recovered()) {
        last = note.h;
        have_last = 1;
    }
    note.magic = 0;
}

void wdt_start(void)
{
#ifndef NO_WDT
    REG(CLKSEL_WDT1) = 1;
    clock_require(CM_WKUP, CM_WKUP_WDT1, CLK_ENABLE);
    write_spr(0xaaaau, 0x5555u);
    wait_posted(WWPS_WCLR | WWPS_WLDR | WWPS_WCRR);
    REG(WDT1 + WDT_WCLR) = 0;
    REG(WDT1 + WDT_WLDR) = 0u - WDT_TIMEOUT_S * WDT_HZ;
    REG(WDT1 + WDT_WCRR) = 0u - WDT_TIMEOUT_S * WDT_HZ;
    wait_posted(WWPS_WCLR | WWPS_WLDR | WWPS_WCRR);
    write_spr(0xbbbbu, 0x4444u);
    kick_at = time_ticks();
    kick_blocks = audio_blocks();
    running = 1;
#endif
}

void wdt_kick(void)
{
    uint32_t now, b;

    if (!running) {
        return;
    }
    now = time_ticks();
    if (now - kick_at < KICK_TICKS) {
        return;
    }
    b = audio_blocks();
    if (b == kick_blocks || (REG(WDT1 + WDT_WWPS) & WWPS_WTGR)) {
        return;
    }
    kick_at = now;
    kick_blocks = b;
    REG(WDT1 + WDT_WTGR) = ++trigger;
}

void wdt_stop(void)
{
    if (!running) {
        return;
    }
    running = 0;
    write_spr(0xaaaau, 0x5555u);
}

int wdt_running(void)
{
    return running;
}

void wdt_note_hang(const struct wdt_hang *h)
{
    note.h = *h;
    note.check = note_check();
    note.magic = HANG_MAGIC;
}

void wdt_clear_hang(void)
{
    note.magic = 0;
}

const struct wdt_hang *wdt_last_hang(void)
{
    return have_last ? &last : 0;
}
