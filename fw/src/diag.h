#ifndef DIAG_H_INCLUDED
#define DIAG_H_INCLUDED

#include <stdint.h>

/* Boot progress on three LEDs, for hardware bring-up without a display:
 * bit 0 = Play Mode 1 SHOT, bit 1 = Play Mode GATED, bit 2 = Loop Mode red.
 * diag_fail() blinks the current stage slowly forever; a CPU exception
 * flashes all three quickly (start.S). */
enum stage {
    STAGE_MAIN = 1,
    STAGE_CLOCK_DOMAINS,
    STAGE_TIMER,
    STAGE_GPMC_CLOCK,
    STAGE_OLED_RESET,
    STAGE_OLED_COMMANDS,
    STAGE_AUDIO,            /* converters, McASP, EDMA bring-up */
};
#define STAGE_MAIN_LOOP 0       /* shown in stall reports, not on the LEDs */

void diag_init(void);
/* Checks the image CRC written by tools/mkapp.py; on mismatch flashes
 * 1 SHOT and Loop Mode alternately and stops. */
void diag_check_image(void);
/* That check block, the last bytes of an image's payload: the magic first,
 * the CRC-32 of everything before it last. */
#define IMAGE_CHECK_MAGIC   0x4b434d41u     /* "AMCK" */
#define IMAGE_CHECK_BYTES   (4u * (2u + 4096u) + 512u + 4u)
/* Bring-up sub-step code on the LEDs (no stage bookkeeping). */
void diag_step(unsigned code);
/* Watchdog: DMTimer3 every 50 ms as a normal IRQ: if diag_alive() isn't
 * called for 3 s it notes the main loop's PC for after WDT1's reset
 * (wdt.h); with WDT1 off, at 8 s it shows it (audio keeps running). As an
 * FIQ (-DWD_FIQ) it could also report a stuck audio IRQ, but the first FIQ
 * hangs the real module (hardware run 25: the boot ROM likely routes FIQ to
 * the secure monitor; the emulator halts on it). Needs the OLED and the interrupt
 * controller; enables IRQs. */
void diag_watchdog_init(void);
void diag_alive(void);
/* Times the main loop went 250 ms without calling diag_alive(). */
extern volatile uint32_t diag_slow_loops;
/* Marks that init is done (stall reports then say STAGE 0). */
void diag_main_loop(void);
void diag_stage(enum stage s);
void diag_leds(unsigned bits);

/* What the main loop and the audio IRQ were last doing, shown by fault and
 * watchdog reports: (DT_* << 16) | detail. Plain stores, cheap enough for
 * the IRQ. */
enum diag_tag {
    DT_NONE,
    /* main loop */
    DT_LOOP, DT_CV_PRESETS, DT_ENGINE_SERVICE, DT_STORAGE_JOB, DT_PRESETS_BG, DT_SAMPLE_HEADER,
    DT_SAMPLE_LOAD, DT_FOLDER_SCAN, DT_FOLDER_PRESETS, DT_FOLDER_LIVE, DT_PRESET_SELECT, DT_REC,
    DT_PANEL, DT_UI_INPUT, DT_UI_DRAW, DT_OLED_FLUSH,
    /* audio IRQ (detail = channel where it applies) */
    DT_IRQ_ENTRY, DT_IRQ_INPUT, DT_IRQ_CV, DT_IRQ_TRIGGERS, DT_IRQ_MACHINE, DT_IRQ_SAMPLER,
    DT_IRQ_SV_ZONE, DT_IRQ_SV_ENV, DT_IRQ_SV_PLAY, DT_IRQ_SV_STEPS, DT_IRQ_SV_BOUNDARY, DT_IRQ_FX,
    DT_IRQ_MIX, DT_IRQ_OUTPUT, DT_IRQ_DONE,
    DT_COUNT
};
extern volatile uint32_t diag_main_tag, diag_irq_tag;
#define DIAG_MAIN(t, d) (diag_main_tag = ((uint32_t)(t) << 16) | ((uint32_t)(d) & 0xffffu))
#ifdef LED_TAGS
/* Debug: the audio IRQ's tag also goes to the LEDs (channel LEDs 1-5 =
 * stage - DT_IRQ_ENTRY + 1, 6-8 = channel; 1 SHOT lit inside the IRQ), so a
 * hang leaves its stage showing. */
void diag_led_tag(uint32_t t, uint32_t d);
#define DIAG_IRQ(t, d)  (diag_irq_tag = ((uint32_t)(t) << 16) | ((uint32_t)(d) & 0xffffu), diag_led_tag(t, d))
#else
#define DIAG_IRQ(t, d)  (diag_irq_tag = ((uint32_t)(t) << 16) | ((uint32_t)(d) & 0xffffu))
#endif

/* Distinct trap codes (UDF immediates) shown by the fault report. */
#define TRAP_SV_READ    0xa001  /* sample read outside the plane + guard */
#define TRAP_SV_PLANE   0xa002  /* voice plane pointer null / outside the pool */
#define TRAP_SV_STALE   0xa003  /* voice plane no longer its sample's */
#define TRAP_SV_POS     0xa004  /* voice positions outside the sample */
#define TRAP_STACK      0xa0ff  /* stack protector */
#define TRAP_TEST       0xa0e0
#define DIAG_TRAP(code) __asm__ volatile("udf %0" :: "i"(code))

/* Masks IRQs, returning the previous state for irq_restore. */
static inline uint32_t irq_save(void)
{
    uint32_t f;

    __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint32_t f)
{
    __asm__ volatile("msr cpsr_c, %0" :: "r"(f) : "memory");
}

/* CPU exception report (from start.S); never returns. kind: 1 undefined,
 * 2 SVC, 3 prefetch abort, 4 data abort; regs = r0-r12, lr; banked = the
 * faulting mode's sp, lr. */
void diag_fault(uint32_t kind, const uint32_t *regs, uint32_t spsr, const uint32_t *banked);
void diag_fail(void) __attribute__((noreturn));
/* As diag_fail(), and once the OLED is up also writes the step and two
 * values (e.g. a register and what it read back) on screen. */
void diag_fail_why(const char *what, uint32_t a, uint32_t b) __attribute__((noreturn));
unsigned diag_current(void);

#endif
