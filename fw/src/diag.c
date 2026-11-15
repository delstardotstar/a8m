#include "diag.h"
#include "hw.h"
#include "oled.h"
#include "update.h"
#include "wdt.h"
#include "systime.h"
#include "board.h"
#include "intc.h"
#include "audio.h"
#include "engine.h"


static unsigned current;

#define CHECK_MAGIC IMAGE_CHECK_MAGIC
#define CHECK_SECTOR 512u
#define CHECK_MAX_SECTORS 4096u

/* Filled in after linking by tools/mkapp.py; volatile so the compiler reads
 * the patched values instead of using the initializer. */
struct image_check {
    uint32_t magic;
    uint32_t sectors;
    uint32_t sector_crc[CHECK_MAX_SECTORS];
    uint32_t sector0[CHECK_SECTOR / 4];  /* reference copy, to show changed words */
    uint32_t total_crc;     /* of everything before this word */
};
_Static_assert(sizeof(struct image_check) == IMAGE_CHECK_BYTES, "image check block size");
const volatile struct image_check image_check __attribute__((section(".imagecheck"), used)) = { CHECK_MAGIC, 0, { 0 }, { 0 }, 0 };
extern const uint8_t _start[];

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;

    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320u & -(c & 1u));
        }
    }
    return ~c;
}

static void spin(volatile uint32_t n);

static void put_hex(char *out, uint32_t v, int digits)
{
    static const char hexd[] = "0123456789ABCDEF";

    for (int i = 0; i < digits; i++) {
        out[i] = hexd[(v >> (4 * (digits - 1 - i))) & 15];
    }
    out[digits] = 0;
}

/* The image came in damaged: show which sectors are bad, as far as the
 * damaged image still lets us, then flash 1 SHOT / Loop Mode. */
static void image_damage_report(uint32_t block_bytes)
{
    char line[40];
    int y = 34, bad = 0;

    clocks_init();
    board_pads_init();
    oled_init();
    oled_clear(0);
    oled_text(2, 2, "IMAGE DAMAGED", rgb565(255, 60, 60), 1);
    put_hex(line, image_check.magic, 8);
    oled_text(2, 12, "MAGIC", rgb565(120, 120, 120), 1);
    oled_text(40, 12, line, rgb565(255, 255, 255), 1);
    put_hex(line, image_check.sectors, 2);
    oled_text(2, 22, "SECTORS", rgb565(120, 120, 120), 1);
    oled_text(52, 22, line, rgb565(255, 255, 255), 1);
    if (image_check.magic == CHECK_MAGIC && image_check.sectors <= CHECK_MAX_SECTORS) {
        for (uint32_t i = 0; i < image_check.sectors; i++) {
            uint32_t from = i * CHECK_SECTOR;
            uint32_t to = from + CHECK_SECTOR < block_bytes ? from + CHECK_SECTOR : block_bytes;
            if (crc32(_start + from, to - from) != image_check.sector_crc[i]) {
                bad++;
                if (y < OLED_H - 8) {
                    put_hex(line, 0x80000000u + from, 8);
                    oled_text(2, y, "BAD", rgb565(255, 190, 40), 1);
                    oled_text(24, y, line, rgb565(255, 255, 255), 1);
                    y += 9;
                }
            }
        }
        put_hex(line, (uint32_t)bad, 2);
        oled_text(80, 22, "BAD", rgb565(120, 120, 120), 1);
        oled_text(102, 22, line, rgb565(255, 255, 255), 1);
        /* Changed words in sector 0: offset, expected, found. */
        const volatile uint32_t *ram = (const volatile uint32_t *)_start;
        int shown = 0, changed = 0;
        y += 2;
        for (uint32_t w = 0; w < CHECK_SECTOR / 4; w++) {
            if (ram[w] == image_check.sector0[w]) {
                continue;
            }
            changed++;
            if (shown < 8 && y < OLED_H - 8) {
                put_hex(line, w * 4, 3);
                oled_text(2, y, line, rgb565(255, 190, 40), 1);
                put_hex(line, image_check.sector0[w], 8);
                oled_text(22, y, line, rgb565(120, 120, 120), 1);
                put_hex(line, ram[w], 8);
                oled_text(74, y, line, rgb565(255, 255, 255), 1);
                y += 9;
                shown++;
            }
        }
        put_hex(line, (uint32_t)changed, 3);
        oled_text(2, OLED_H - 9, "WORDS CHANGED", rgb565(120, 120, 120), 1);
        oled_text(86, OLED_H - 9, line, rgb565(255, 255, 255), 1);
    }
    oled_flush();
}

/* MLO's value at 0x80000034 (start.S), saved before the check restores the
 * linked value. */
extern volatile uint32_t mlo_boot_word;
uint32_t mlo_boot_value;

void diag_check_image(void)
{
    const uint8_t *block = (const uint8_t *)&image_check;
    const uint8_t *end = (const uint8_t *)&image_check.total_crc;

    mlo_boot_value = mlo_boot_word;
    mlo_boot_word = 0;

    if (image_check.magic == CHECK_MAGIC &&
        crc32(_start, (uint32_t)(end - _start)) == image_check.total_crc) {
        return;
    }
    image_damage_report((uint32_t)(block - _start));
    for (;;) {
        diag_leds(1);
        spin(4000000);
        diag_leds(4);
        spin(4000000);
    }
}

void diag_step(unsigned code)
{
    diag_leds(code);
}

#ifdef LED_TAGS
void diag_led_tag(uint32_t t, uint32_t d)
{
    uint32_t code = ((t - DT_IRQ_ENTRY + 1u) & 31u) | ((d & 7u) << 5);
    uint32_t on2 = (code & 0x7fu) << 11, off2 = (~code & 0x7fu) << 11;

    if (t == DT_IRQ_DONE) {
        off2 |= 1u << LED_A_PIN;
    } else {
        on2 |= 1u << LED_A_PIN;
    }
    REG(GPIO2 + GPIO_SET) = on2;
    REG(GPIO2 + GPIO_CLEAR) = off2;
    REG(GPIO0 + ((code & 0x80u) ? GPIO_SET : GPIO_CLEAR)) = 1u << 8;
}
#endif

void diag_leds(unsigned bits)
{
    REG(GPIO2 + ((bits & 1) ? GPIO_SET : GPIO_CLEAR)) = 1u << LED_A_PIN;
    REG(GPIO2 + ((bits & 2) ? GPIO_SET : GPIO_CLEAR)) = 1u << LED_B_PIN;
    REG(GPIO0 + ((bits & 4) ? GPIO_SET : GPIO_CLEAR)) = 1u << LED_LOOP_RED_PIN;
}

/* Clock enables here must not hang: if one times out, diag_fail() shows
 * the stage we were in. */
void diag_init(void)
{
    if (!clock_enable(CM_WKUP, CM_WKUP_GPIO0, CLK_ENABLE_DBCLK) ||
        !clock_enable(CM_PER, CM_PER_GPIO2, CLK_ENABLE_DBCLK)) {
        for (;;) {
        }
    }
    REG(CONTROL + PAD_LED_A) = PAD_MODE7_OUT;
    REG(CONTROL + PAD_LED_B) = PAD_MODE7_OUT;
    REG(CONTROL + PAD_LED_LOOP_RED) = PAD_MODE7_OUT;
    REG(GPIO2 + GPIO_OE) &= ~((1u << LED_A_PIN) | (1u << LED_B_PIN));
    REG(GPIO0 + GPIO_OE) &= ~(1u << LED_LOOP_RED_PIN);
}

void diag_stage(enum stage s)
{
    current = s;
    diag_leds(s);
}

void diag_main_loop(void)
{
    current = STAGE_MAIN_LOOP;
}

void clock_require(uint32_t module, uint32_t offset, uint32_t value)
{
    if (!clock_enable(module, offset, value)) {
        diag_fail_why("CLOCK ENABLE", module + offset, REG(module + offset));
    }
}

void domain_require(uint32_t module, uint32_t offset)
{
    if (!domain_wake(module, offset)) {
        diag_fail_why("CLOCK DOMAIN", module + offset, REG(module + offset));
    }
}

/* Interconnect clock domains, in the order the stock app wakes them. */
/* Each step shows its code (1-7) on the LEDs first, so a hang names it. */
void clocks_init(void)
{
    diag_step(1);
    domain_require(CM_PER, CM_PER_L3S_CLKSTCTRL);
    diag_step(2);
    domain_require(CM_PER, CM_PER_L3_CLKSTCTRL);
    diag_step(3);
    clock_require(CM_PER, CM_PER_L3_INSTR_CLKCTRL, CLK_ENABLE);
    diag_step(4);
    clock_require(CM_PER, CM_PER_L3_CLKCTRL, CLK_ENABLE);
    diag_step(5);
    domain_require(CM_PER, CM_PER_OCPWP_L3_CLKSTCTRL);
    diag_step(6);
    domain_require(CM_PER, CM_PER_L4LS_CLKSTCTRL);
    diag_step(7);
    clock_require(CM_PER, CM_PER_L4LS_CLKCTRL, CLK_ENABLE);
}

unsigned diag_current(void)
{
    return current;
}

static void spin(volatile uint32_t n)
{
    while (n--) {
    }
}

static void hex32(char *out, uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";

    out[0] = '0';
    out[1] = 'X';
    for (int i = 0; i < 8; i++) {
        out[2 + i] = digits[(v >> (28 - 4 * i)) & 15];
    }
    out[10] = 0;
}

void diag_fail_why(const char *what, uint32_t a, uint32_t b)
{
    if (oled_ready) {
        char num[12] = "STAGE 0";

        num[6] = (char)('0' + current);
        oled_clear(0);
        oled_text(2, 2, "FAILED", rgb565(255, 60, 60), 2);
        oled_text(2, 22, fw_version, rgb565(120, 120, 120), 1);
        oled_text(2, 36, num, rgb565(255, 255, 255), 1);
        oled_text(2, 50, what, rgb565(255, 190, 40), 1);
        hex32(num, a);
        oled_text(2, 64, num, rgb565(255, 255, 255), 1);
        hex32(num, b);
        oled_text(2, 76, num, rgb565(255, 255, 255), 1);
        oled_flush();
    }
    diag_fail();
}

void diag_fail(void)
{
    wdt_stop();
    for (;;) {
        diag_leds(current);
        spin(40000000);
        diag_leds(0);
        spin(40000000);
    }
}

volatile uint32_t diag_main_tag, diag_irq_tag;

static const char *const tag_names[DT_COUNT] = {
    [DT_NONE] = "-", [DT_LOOP] = "LOOP", [DT_CV_PRESETS] = "CV PRESETS",
    [DT_ENGINE_SERVICE] = "ENGINE SVC", [DT_STORAGE_JOB] = "STORAGE JOB",
    [DT_PRESETS_BG] = "PRESETS BG", [DT_SAMPLE_HEADER] = "SMP HEADER",
    [DT_SAMPLE_LOAD] = "SMP LOAD", [DT_FOLDER_SCAN] = "FOLDER SCAN",
    [DT_FOLDER_PRESETS] = "FOLDER PRST", [DT_FOLDER_LIVE] = "FOLDER LIVE",
    [DT_PRESET_SELECT] = "PRESET SEL", [DT_REC] = "REC", [DT_PANEL] = "PANEL",
    [DT_UI_INPUT] = "UI INPUT", [DT_UI_DRAW] = "UI DRAW", [DT_OLED_FLUSH] = "OLED FLUSH",
    [DT_IRQ_ENTRY] = "ENTRY", [DT_IRQ_INPUT] = "INPUT", [DT_IRQ_CV] = "CV",
    [DT_IRQ_TRIGGERS] = "TRIGGERS", [DT_IRQ_MACHINE] = "MACHINE", [DT_IRQ_SAMPLER] = "SAMPLER",
    [DT_IRQ_SV_ZONE] = "SV ZONE", [DT_IRQ_SV_ENV] = "SV ENV", [DT_IRQ_SV_PLAY] = "SV PLAY",
    [DT_IRQ_SV_STEPS] = "SV STEPS", [DT_IRQ_SV_BOUNDARY] = "SV BOUNDARY", [DT_IRQ_FX] = "FX", [DT_IRQ_MIX] = "MIX",
    [DT_IRQ_OUTPUT] = "OUTPUT", [DT_IRQ_DONE] = "DONE",
};

static const char *mode_name(uint32_t psr)
{
    switch (psr & 31) {
    case 0x10: return "USR";
    case 0x11: return "FIQ";
    case 0x12: return "IRQ";
    case 0x13: return "SVC";
    case 0x17: return "ABT";
    case 0x1b: return "UND";
    case 0x1f: return "SYS";
    default:   return "???";
    }
}

/* Report lines: 9 px each; pairs of "LL XXXXXXXX" in two columns. */
static int ry;

static void r_text(const char *s, uint16_t c)
{
    oled_text(2, ry, s, c, 1);
    ry += 9;
}

static void r_hex(int col, const char *label, uint32_t v)
{
    char line[12];
    int x = col ? 66 : 2;

    put_hex(line, v, 8);
    oled_text(x, ry, label, rgb565(255, 190, 40), 1);
    oled_text(x + 13, ry, line, rgb565(255, 255, 255), 1);
}

static void r_pair(const char *a, uint32_t va, const char *b, uint32_t vb)
{
    r_hex(0, a, va);
    r_hex(1, b, vb);
    ry += 9;
}

static void r_tag(const char *label, uint32_t tag)
{
    char line[8];
    unsigned t = tag >> 16;

    oled_text(2, ry, label, rgb565(255, 190, 40), 1);
    oled_text(26, ry, t < DT_COUNT && tag_names[t] ? tag_names[t] : "?", rgb565(255, 255, 255), 1);
    put_hex(line, tag & 0xffffu, 4);
    oled_text(103, ry, line, rgb565(120, 120, 120), 1);
    ry += 9;
}

static int r_begin(const char *title)
{
    wdt_stop();
    if (!oled_panic()) {
        return 0;
    }
    oled_clear(0);
    ry = 1;
    r_text(title, rgb565(255, 60, 60));
    r_text(fw_version, rgb565(120, 120, 120));
    return 1;
}

static void r_where(uint32_t pc, uint32_t psr, uint32_t lr, uint32_t sp)
{
    r_hex(0, "PC", pc);
    oled_text(66, ry, "IN", rgb565(255, 190, 40), 1);
    oled_text(79, ry, mode_name(psr), rgb565(255, 255, 255), 1);
    oled_text(103, ry, psr & 0x80u ? "I" : "", rgb565(120, 120, 120), 1);
    oled_text(110, ry, psr & 0x40u ? "F" : "", rgb565(120, 120, 120), 1);
    ry += 9;
    r_pair("LR", lr, "SP", sp);
}

static void r_tags(void)
{
    r_tag("MN", diag_main_tag);
    r_tag("IQ", diag_irq_tag);
}

/* The tagged channel's sample voice (engine_diag). */
static void r_voice(void)
{
    uint32_t w[4];
    unsigned t = diag_irq_tag >> 16;

    if (t < DT_IRQ_SAMPLER || t > DT_IRQ_SV_BOUNDARY) {
        return;
    }
    engine_diag((int)(diag_irq_tag & 7u), w);
    r_pair("VX", w[0], "VP", w[1]);
    r_pair("VL", w[2], "VF", w[3]);
}

static void halt_blink(void) __attribute__((noreturn));
static void halt_blink(void)
{
    wdt_stop();
    for (;;) {
        diag_leds(7);
        spin(4000000);
        diag_leds(0);
        spin(4000000);
    }
}

void diag_fault(uint32_t kind, const uint32_t *regs, uint32_t spsr, const uint32_t *banked)
{
    static const char *const kinds[5] = { "FAULT", "UNDEFINED", "SVC", "PREFETCH ABORT", "DATA ABORT" };
    static int faulting;
    uint32_t lr = regs[13], pc, fsr = 0, far = 0, insn = 0;
    char title[24];

    if (faulting++) {
        halt_blink();
    }
    pc = lr - (kind == 4 ? 8u : kind == 1 && (spsr & 0x20u) ? 2u : 4u);
    if (kind == 4) {
        __asm__ volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(fsr));
        __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(far));
    } else if (kind == 3) {
        __asm__ volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(fsr));
        __asm__ volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(far));
    }
    const char *k = kinds[kind < 5 ? kind : 0];
    unsigned n = 0;
    for (; k[n]; n++) {
        title[n] = k[n];
    }
    title[n] = 0;
    if (kind == 1 && !(spsr & 0x20u)) {
        insn = *(const volatile uint32_t *)pc;
        if ((insn & 0x0ff000f0u) == 0x07f000f0u) {
            memcpy(title, "TRAP ", 5);
            put_hex(title + 5, ((insn >> 4) & 0xfff0u) | (insn & 0xfu), 4);
        }
    }
    if (r_begin(title)) {
        r_where(pc, spsr, banked[1], banked[0]);
        r_pair("PS", spsr, kind == 1 ? "IN" : "FS", kind == 1 ? insn : fsr);
        r_pair("FA", far, "IP", irq_interrupted_pc);
        r_pair("R0", regs[0], "R1", regs[1]);
        r_pair("R2", regs[2], "R3", regs[3]);
        r_tags();
        r_voice();
        oled_flush();
    }
    halt_blink();
}

#ifdef A8_DEBUG
void __stack_chk_fail(void) __attribute__((noreturn));
void __stack_chk_fail(void)
{
    DIAG_TRAP(TRAP_STACK);
    for (;;) {
    }
}
uint32_t __stack_chk_guard = 0x000aff0du;
#endif

/* DMTimer3: the watchdog FIQ every 50 ms. */
#define DMTIMER3        0x48042000u
#define CM_PER_TIMER3   0x084u
#define CM_DPLL_CLKSEL_TIMER3 0x44e0050cu
#define TIMER_IRQSTATUS 0x028u
#define TIMER_IRQENABLE_SET 0x02cu
#define TIMER_TCLR      0x038u
#define TIMER_TCRR      0x03cu
#define TIMER_TLDR      0x040u
#define TIMER_OVF       0x2u
#define WD_TICKS        1200000u        /* 50 ms at 24 MHz */
#define WD_AUDIO_STUCK  6               /* ticks without an audio block */
#define WD_SLOW         5               /* 250 ms: counts in diag_slow_loops */
#define WD_STALLED      160             /* 8 s: main loop reported (WDT1 off) */
#define WD_HANG_NOTE    60              /* 3 s: noted for after WDT1's reset */

static volatile uint32_t alive;
volatile uint32_t diag_slow_loops;

void diag_alive(void)
{
    alive++;
}

/* From start.S: the interrupted context. */
struct fiq_frame {
    uint32_t sp, lr, spsr;      /* the interrupted mode's */
    uint32_t r[8];
    uint32_t pc;
};

static void watchdog_report(const char *title, const struct fiq_frame *f, uint32_t ticks)
{
    if (!r_begin(title)) {
        return;
    }
    r_where(f->pc, f->spsr, f->lr, f->sp);
    r_pair("PS", f->spsr, "IP", irq_interrupted_pc);
    r_pair("R0", f->r[0], "R1", f->r[1]);
    r_pair("BK", audio_blocks(), "TK", ticks);
    r_tags();
    r_voice();
    oled_flush();
}

void diag_fiq(const struct fiq_frame *f)
{
    static uint32_t seen_alive, seen_blocks, still, audio_still;
    uint32_t b = audio_blocks();

    REG(DMTIMER3 + TIMER_IRQSTATUS) = TIMER_OVF;
    (void)REG(DMTIMER3 + TIMER_IRQSTATUS);      /* posted write (hardware run 14) */
    intc_fiq_done();
    if (b && b == seen_blocks) {
        if (++audio_still >= WD_AUDIO_STUCK) {
            uint32_t m = f->spsr & 31u;
            watchdog_report(m == 0x12 || m == 0x1f ? "AUDIO IRQ STUCK" : (f->spsr & 0x80u) ? "IRQS MASKED" : "AUDIO STOPPED",
                            f, audio_still);
            halt_blink();
        }
    } else {
        audio_still = 0;
    }
    seen_blocks = b;
    if (alive != seen_alive) {
        seen_alive = alive;
        if (still >= WD_HANG_NOTE) {
            wdt_clear_hang();
        }
        still = 0;
        return;
    }
    if (++still == WD_SLOW) {
        diag_slow_loops++;
    }
    if (still == WD_HANG_NOTE) {
        struct wdt_hang h = { f->pc, irq_interrupted_pc, f->lr, f->sp, diag_main_tag, diag_irq_tag, b,
                              (uint32_t)(time_us64() / 1000u) };
        wdt_note_hang(&h);
    }
    /* Drawing blocks audio for a whole flush, so only for a dead loop.
     * With WDT1 running the reset comes first. */
    if (still >= WD_STALLED && still % 20u == 0 && !wdt_running()) {
        watchdog_report("MAIN LOOP STALLED", f, still);
    }
}

#ifndef WD_FIQ
static void wd_irq(void)
{
    struct fiq_frame fr = { 0 };

    fr.spsr = 0x13u;
    fr.pc = irq_interrupted_pc;
    diag_fiq(&fr);
}
#endif

void diag_watchdog_init(void)
{
    REG(CM_DPLL_CLKSEL_TIMER3) = 1;                 /* 24 MHz oscillator */
    clock_require(CM_PER, CM_PER_TIMER3, CLK_ENABLE);
    REG(DMTIMER3 + TIMER_TCLR) = 0;
    REG(DMTIMER3 + TIMER_TLDR) = 0xffffffffu - WD_TICKS + 1u;
    REG(DMTIMER3 + TIMER_TCRR) = 0xffffffffu - WD_TICKS + 1u;
    REG(DMTIMER3 + TIMER_IRQSTATUS) = TIMER_OVF;
    REG(DMTIMER3 + TIMER_IRQENABLE_SET) = TIMER_OVF;
#ifndef WD_FIQ
    intc_enable(IRQ_TIMER3, wd_irq);
    REG(DMTIMER3 + TIMER_TCLR) = 0x3;               /* start, auto-reload */
#else
    intc_enable_fiq(IRQ_TIMER3);
    REG(DMTIMER3 + TIMER_TCLR) = 0x3;               /* start, auto-reload */
    __asm__ volatile("cpsie f" ::: "memory");
#endif
    irq_on();
}
