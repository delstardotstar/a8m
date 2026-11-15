#include "ui.h"
#include "audio.h"
#include "cv.h"
#include "diag.h"
#include "engine.h"
#include "font.h"
#include "gfx.h"
#include "hw.h"
#include "leds.h"
#include "mmc.h"
#include "ops.h"
#include "preset.h"
#include "r70.h"
#include "r80.h"
#include "rec.h"
#include "samples.h"
#include "storage.h"
#include "systime.h"
#include "version.h"
#include "yml.h"
#include "voices.h"
#include "wave.h"
#include "eq.h"
#include "fx.h"
#include "mix_fx.h"
#include "grains.h"
#include "tlock.h"
#include "update.h"
#include "cal.h"
#include "health.h"
#include "prof.h"

#include <stddef.h>
#include <string.h>

enum page {
    P_HOME, P_CHANNELS, P_MACHINE, P_MACHINE_CV,
    P_PITCH, P_LEVEL, P_PANMIX, P_MUTATE, P_ENVELOPE, P_PHASE, P_SAMPLER_ONLY, P_ZONES, P_SPOS, P_LPOS,
    P_EQ, P_EQCV, P_FILTER, P_COMP, P_SENDS, P_REVERB, P_DELAY, P_MIXCOMP, P_GRAIN, P_GRAINCV, P_TLOCK,
    P_UTIL, P_CVMON, P_PANEL, P_AUDIO, P_BENCH, P_PREFS, P_INCDEC, P_ABOUT, P_LOAD, P_SAVE,
    P_FNAME, P_FUTIL, P_PROGRESS, P_MEMORY, P_SDCARD, P_IMPORT, P_RSETUP, P_SOFTWARE, P_CAL,
    P_COUNT
};

/* ---- Fields: registered while drawing, used by the next input pass. ---- */

struct field {
    int (*get)(int arg);
    void (*set)(int arg, int v);
    void (*range)(int arg, int *min, int *max);
    void (*fmt)(char *out, int v, int arg);
    void (*action)(int arg);    /* click runs this instead of fine edit */
    uint8_t list;               /* click opens this popup instead (L_*) */
    const char *const *labels;  /* popup names by value, else fmt */
    void (*hold)(int arg);      /* long press on DATA1 */
};

enum { L_NONE, L_MACHINE, L_CVSRC, L_D2CV, L_VLIST, L_MENU, L_PICK, L_SAMPLE, L_REVERB, L_RVPROG };

struct field_ref {
    const struct field *f;
    int arg;
};

#define MAX_FIELDS 128

static struct field_ref fields[MAX_FIELDS];
static int nfields, drawn_page = -1;
#define LONG_PRESS_US 2000000u
#define DOUBLE_PRESS_US 600000u         /* stock's double-press window */

static int page = P_HOME, sel;
/* A Stereo/Right channel's view of its parameter pages: sel is its master,
 * rview the Stereo/Right channel, whose offsets the pages' values edit
 * (-1: none). */
static int rview = -1;
static const struct field *rofs_sub(const struct field *f, int *arg);
static void rofs_mark(int x, int y, int k);
static int rofs_pitch(const struct field_ref *r);
static const struct param_choice *rofs_choice(const struct field_ref *r);
static int rofs_value(int d);
static int focus[P_COUNT];
static int fine;
static int d2_ref, d2_engaged, d2_now, d2_last;

/* DATA2 as an absolute position in a list of n: -1 until it has moved past
 * the pickup window (DATA1 steps release it by clearing d2_engaged) and
 * while it rests. */
static int d2_jump(int d, int n)
{
    if (n < 2) {
        return -1;
    }
    if (!d2_engaged && (d > d2_ref + 48 || d + 48 < d2_ref)) {
        d2_engaged = 1;
        d2_last = -100;
    }
    if (!d2_engaged || (d <= d2_last + 2 && d + 2 >= d2_last)) {
        return -1;
    }
    d2_last = d;
    return (int)(((int64_t)(n - 1) * d + 2047) / 4095);
}
static const char *sampler_only_title = "";
static char toast[24];
static uint64_t toast_until;
static int cv_raw_view;
static uint32_t sd_total_kb, sd_free_kb;
static int init_armed;
static uint32_t channels_held_used;
static int play_held_used, play_d2_used, play_d2_ref;
static const struct panel *last_panel;
static uint32_t loop_us_max_seen;

uint32_t ui_loop_ms_max(void)
{
    return loop_us_max_seen / 1000u;
}

void ui_say_for(const char *msg, uint32_t us)
{
    unsigned i = 0;

    for (; msg[i] && i < sizeof(toast) - 1; i++) {
        toast[i] = msg[i];
    }
    toast[i] = 0;
    toast_until = time_us64() + us;
}

void ui_say(const char *msg)
{
    ui_say_for(msg, 1500000u);
}

/* ---- Machine bench: each voice alone on channel 1, its render time. ---- */

#define BENCH_SETTLE_US 200000u    /* after a switch: init (table fills) happens here */
#define BENCH_WARM_US   150000u
#define BENCH_MEASURE_US 400000u

static struct preset bench_saved;
static int bench_voice = -1;       /* -1 idle, else voice being measured */
static uint32_t bench_t0, bench_us[VOICE_COUNT];
static int bench_measuring, bench_done;

static void bench_select(int v)
{
    __asm__ volatile("cpsid i" ::: "memory");
    for (int ch = 0; ch < CHANNELS; ch++) {
        preset.ch[ch].machine = 0;
    }
    preset_set_machine(&preset.ch[0], v);
    preset.ch[0].pitch = 0;
    preset.ch[0].level = 0;
    preset.ch[0].bits = BITS_MAX;
    __asm__ volatile("cpsie i" ::: "memory");
    bench_voice = v;
    bench_t0 = time_us();
    bench_measuring = 0;
}

static void bench_start(int a)
{
    (void)a;
    if (bench_voice >= 0) {
        return;
    }
    bench_saved = preset;
    bench_done = 0;
    bench_select(0);
}

static void bench_tick(void)
{
    uint32_t t;

    if (bench_voice < 0) {
        return;
    }
    t = time_us() - bench_t0;
    /* A first hit warms the caches (and is not measured); the second is. */
    if (bench_measuring == 0 && t >= BENCH_SETTLE_US) {
        engine_trigger(0);
        bench_measuring = 1;
    } else if (bench_measuring == 1 && t >= BENCH_SETTLE_US + BENCH_WARM_US) {
        engine_status.render_us_max = 0;
        engine_trigger(0);
        bench_measuring = 2;
    } else if (bench_measuring == 2 && t >= BENCH_SETTLE_US + BENCH_WARM_US + BENCH_MEASURE_US) {
        bench_us[bench_voice] = engine_status.render_us_max;
        if (bench_voice + 1 < VOICE_COUNT) {
            bench_select(bench_voice + 1);
            return;
        }
        __asm__ volatile("cpsid i" ::: "memory");
        preset = bench_saved;
        __asm__ volatile("cpsie i" ::: "memory");
        bench_voice = -1;
        bench_done = 1;
    }
}

/* ---- Formatting ---- */

static char *put_uint(char *o, unsigned v, int min_digits)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v || n < min_digits);
    while (n) {
        *o++ = tmp[--n];
    }
    *o = 0;
    return o;
}

/* v / 10^dec with sign, e.g. fixed(-125, 2, 2) = "-01.25". */
static void fmt_fixed(char *o, int v, int dec, int int_digits, int plus)
{
    unsigned u = (unsigned)(v < 0 ? -v : v), p = 1;

    for (int i = 0; i < dec; i++) {
        p *= 10;
    }
    if (v < 0) {
        *o++ = '-';
    } else if (plus) {
        *o++ = '+';
    }
    o = put_uint(o, u / p, int_digits);
    if (dec) {
        *o++ = '.';
        put_uint(o, u % p, dec);
    }
}

static void fmt_int(char *o, int v)
{
    if (v < 0) {
        *o++ = '-';
        v = -v;
    }
    put_uint(o, (unsigned)v, 1);
}

static void cat(char *o, const char *s)
{
    while (*o) {
        o++;
    }
    while ((*o++ = *s++)) {
    }
}

/* At most n chars of s. */
static void copy_n(char *o, const char *s, int n)
{
    int i = 0;

    for (; s[i] && i < n; i++) {
        o[i] = s[i];
    }
    o[i] = 0;
}

static int strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static const char *machine_name(int m)
{
    return m > 0 && m < VOICE_COUNT ? voices[m]->name : "OFF";
}

static void fmt_src(char *o, int src)
{
    static const char *const rel[] = { "OFF", "CV A", "CV B", "CV C" };

    if (src < CV_ABS) {
        memcpy(o, rel[src], 5);
        return;
    }
    src -= CV_ABS;
    o[0] = (char)('1' + src / 3);
    o[1] = (char)('A' + src % 3);
    o[2] = 0;
}

static const struct param_choice *dest_choice(int d)
{
    const struct voice_desc *v = voices[preset.ch[sel].machine];
    int p = d - D_CTRL;

    return v && p >= 0 && p < (int)v->nparams ? v->params[p].choice : 0;
}

static void fmt_dest(char *o, int v, int d)
{
    switch (d) {
    case D_PITCH:
        fmt_fixed(o, v, 2, 2, 1);
        cat(o, " SEMI");
        return;
    case D_LEVEL:
    case D_MIX:
        if (v <= (d == D_MIX ? MIX_MIN : LEVEL_MIN)) {
            memcpy(o, "OFF", 4);
            return;
        }
        fmt_fixed(o, v, 1, 2, 1);
        cat(o, " DB");
        return;
    case D_PAN:
        fmt_fixed(o, v, 2, 1, 1);
        return;
    case D_BITS:
        if (v >= BITS_MAX) {
            memcpy(o, "OFF", 4);
            return;
        }
        fmt_fixed(o, v, 1, 2, 0);
        cat(o, " BITS");
        return;
    default:
        if (d >= D_CTRL) {
            const struct param_choice *ch = dest_choice(d);

            if (ch) {
                o[0] = 0;
                cat(o, ch->labels[param_choice_index(ch, v)]);
                return;
            }
        }
        fmt_int(o, v);
    }
}

/* ---- Field descriptors ---- */

static int16_t *dest_base(int ch, int d)
{
    struct channel_cfg *c = &preset.ch[ch];

    switch (d) {
    case D_PITCH: return &c->pitch;
    case D_LEVEL: return &c->level;
    case D_PAN:   return &c->pan;
    case D_MIX:   return &c->mix;
    case D_BITS:  return &c->bits;
    default:      return 0;
    }
}

static void dest_range(int d, int *lo, int *hi)
{
    switch (d) {
    case D_PITCH: *lo = -2400; *hi = 2400; return;
    case D_LEVEL:
    case D_MIX:   *lo = MIX_MIN; *hi = LEVEL_MAX; return;
    case D_PAN:   *lo = -100; *hi = 100; return;
    case D_BITS:  *lo = BITS_MIN; *hi = BITS_MAX; return;
    default:      *lo = 0; *hi = 127; return;
    }
}

static int get_dest(int d)
{
    int16_t *b = dest_base(sel, d);

    return b ? *b : preset.ch[sel].ctrl[d - D_CTRL];
}

static void set_dest(int d, int v)
{
    int16_t *b = dest_base(sel, d);

    if (b) {
        *b = (int16_t)v;
    } else {
        preset.ch[sel].ctrl[d - D_CTRL] = (uint8_t)v;
    }
}

static const struct field f_dest = { get_dest, set_dest, dest_range, fmt_dest, 0, L_NONE, 0, 0 };

static int get_src(int d) { return preset.ch[sel].mod[d].src; }
static void set_src(int d, int v) { preset.ch[sel].mod[d].src = (uint8_t)v; }
static void range_src(int d, int *lo, int *hi) { (void)d; *lo = 0; *hi = CV_SRC_COUNT - 1; }
static void fmt_src_field(char *o, int v, int d) { (void)d; fmt_src(o, v); }
static const struct field f_src = { get_src, set_src, range_src, fmt_src_field, 0, L_CVSRC, 0, 0 };

static int get_amt(int d) { return preset.ch[sel].mod[d].amt; }
static void set_amt(int d, int v) { preset.ch[sel].mod[d].amt = (int16_t)v; }
static void range_amt(int d, int *lo, int *hi) { (void)d; *lo = -AMT_MAX; *hi = AMT_MAX; }
static void fmt_amt(char *o, int v, int d) { (void)d; fmt_fixed(o, v, 2, 1, 1); }
static const struct field f_amt = { get_amt, set_amt, range_amt, fmt_amt, 0, L_NONE, 0, 0 };

static int get_machine(int ch) { return preset.ch[ch].machine; }
static void set_machine(int ch, int v)
{
    __asm__ volatile("cpsid i" ::: "memory");
    preset_set_machine(&preset.ch[ch], v);
    for (int r = ch + 1; r < CHANNELS && preset.ch[r].mode == MODE_STEREO; r++) {
        memset(preset.ch[r].ofs + PO_CTRL, 0, CONTROLS * sizeof(preset.ch[r].ofs[0]));
    }
    __asm__ volatile("cpsie i" ::: "memory");
}
static void range_machine(int ch, int *lo, int *hi) { (void)ch; *lo = 0; *hi = VOICE_COUNT - 1; }
static void fmt_machine(char *o, int v, int ch)
{
    const char *n = machine_name(v);
    int i = 0;

    (void)ch;
    for (; n[i] && i < 8; i++) {
        o[i] = n[i];
    }
    o[i] = 0;
}
static void hold_machine(int ch);
static const struct field f_machine = { get_machine, set_machine, range_machine, fmt_machine, 0, L_MACHINE, 0,
                                        hold_machine };

static int get_mix_on(int ch) { return preset.ch[ch].mix_on; }
static void set_mix_on(int ch, int v) { preset.ch[ch].mix_on = (uint8_t)v; }
static void range_onoff(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = 1; }
static void fmt_onoff(char *o, int v, int a) { (void)a; memcpy(o, v ? "ON" : "OFF", 4); }
static const struct field f_mix_on = { get_mix_on, set_mix_on, range_onoff, fmt_onoff, 0, L_VLIST, 0, 0 };

static int get_mode(int ch) { return preset.ch[ch].mode; }
static void set_mode(int ch, int v) { preset.ch[ch].mode = (uint8_t)v; }
static void range_mode(int ch, int *lo, int *hi) { *lo = 0; *hi = ch ? MODE_COUNT - 1 : 0; }
static void fmt_mode(char *o, int v, int ch)
{
    static const char *const names[MODE_COUNT] = { "MSTR", "LINK", "STER", "CYCL" };
    (void)ch;
    memcpy(o, names[v], 5);
}
static const char *const mode_labels[MODE_COUNT] = { "MASTER", "LINK", "STEREO/RIGHT", "CYCLE" };
static const struct field f_mode = { get_mode, set_mode, range_mode, fmt_mode, 0, L_VLIST, mode_labels, 0 };

static int get_d2cv(int a) { (void)a; return preset.d2cv; }
static void set_d2cv(int a, int v) { (void)a; preset.d2cv = (uint8_t)v; }
static void range_d2cv(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = CV_INPUTS; }
static void fmt_d2cv(char *o, int v, int a)
{
    (void)a;
    if (!v) {
        memcpy(o, "OFF", 4);
    } else {
        fmt_src(o, CV_ABS + v - 1);
    }
}
static const struct field f_d2cv = { get_d2cv, set_d2cv, range_d2cv, fmt_d2cv, 0, L_D2CV, 0, 0 };

/* Crossfade groups: a channel's letter on CHANNELS; the bottom row shows
 * the focused channel's group (or the one picked there: CH buttons, DATA1)
 * with its CV and width (stock). */
#define XF_ROW_FIELDS   4
#define XF_FIELD_GROUP  (CHANNELS * XF_ROW_FIELDS + 1)
static int xf_show = -1;

static const char *const xgroup_labels[] = { "-", "A", "B", "C", "D" };
static int get_xgroup(int ch) { return preset.ch[ch].s.xfade; }
static void set_xgroup(int ch, int v) { preset.ch[ch].s.xfade = (uint8_t)v; xf_show = v - 1; }
static void range_xgroup(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = XFADE_GROUPS; }
static void fmt_xgroup(char *o, int v, int a) { (void)a; o[0] = xgroup_labels[v][0]; o[1] = 0; }
static void hold_xgroup(int ch);
static void focus_moved(void);
static int (*fx_coarse(const struct field *f))(int, int, int);
static const struct field f_xgroup = { get_xgroup, set_xgroup, range_xgroup, fmt_xgroup, 0, L_VLIST, xgroup_labels,
                                       hold_xgroup };

static int get_xfsel(int a) { (void)a; return xf_show + 1; }
static void set_xfsel(int a, int v) { (void)a; xf_show = v - 1; }
static void range_xfsel(int a, int *lo, int *hi) { (void)a; *lo = 1; *hi = XFADE_GROUPS; }
static const struct field f_xfsel = { get_xfsel, set_xfsel, range_xfsel, fmt_xgroup, 0, L_NONE, 0, 0 };

static int get_xfcv(int a) { (void)a; return xf_show < 0 ? 0 : preset.xfade_cv[xf_show]; }
static void set_xfcv(int a, int v) { (void)a; if (xf_show >= 0) preset.xfade_cv[xf_show] = (uint8_t)v; }
static void fmt_d2cv(char *o, int v, int a);
static void range_d2cv(int a, int *lo, int *hi);
static const struct field f_xfcv = { get_xfcv, set_xfcv, range_d2cv, fmt_d2cv, 0, L_D2CV, 0, 0 };

/* Width steps: 0.01 V to 0.99 V, then 0.1 V to 10 V. */
static int xfw_index(int w) { return w < 100 ? w - 1 : 99 + (w - 100) / 10; }
static int xfw_value(int i) { return i < 99 ? i + 1 : 100 + (i - 99) * 10; }
static int get_xfw(int a) { (void)a; return xfw_index(xf_show < 0 ? XFW_DEFAULT : preset.xfade_width[xf_show]); }
static void set_xfw(int a, int v) { (void)a; if (xf_show >= 0) preset.xfade_width[xf_show] = (int16_t)xfw_value(v); }
static void range_xfw(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = xfw_index(XFW_MAX); }
static void fmt_xfw(char *o, int v, int a);
static const struct field f_xfw = { get_xfw, set_xfw, range_xfw, fmt_xfw, 0, L_NONE, 0, 0 };

/* XF quick-jump: to the group's CV field (also from "-", as stock). */
static void hold_xgroup(int ch)
{
    xf_show = preset.ch[ch].s.xfade - 1;
    focus[P_CHANNELS] = XF_FIELD_GROUP + 1;
    focus_moved();
    ui_say("1-8 SELECTS CHANNEL");
}

static int get_pref_preview(int a) { (void)a; return pref_browse_preview; }
static void set_pref_preview(int a, int v) { (void)a; pref_browse_preview = v; storage_prefs_changed(); }
static const struct field f_pref_preview = { get_pref_preview, set_pref_preview, range_onoff, fmt_onoff, 0, L_VLIST, 0, 0 };

static int get_pref_aud(int a) { (void)a; return pref_auto_audition; }
static void set_pref_aud(int a, int v) { (void)a; pref_auto_audition = v; storage_prefs_changed(); }
static const struct field f_pref_aud = { get_pref_aud, set_pref_aud, range_onoff, fmt_onoff, 0, L_VLIST, 0, 0 };

static int get_pref_load(int a) { (void)a; return pref_load_clean; }
static void set_pref_load(int a, int v) { (void)a; pref_load_clean = v; storage_prefs_changed(); }
static const char *const load_labels[] = { "CARRY", "CLEAN" };
static void fmt_load(char *o, int v, int a) { (void)a; o[0] = 0; cat(o, load_labels[v & 1]); }
static const struct field f_pref_load = { get_pref_load, set_pref_load, range_onoff, fmt_load, 0, L_VLIST, load_labels, 0 };

static int get_pref_warn(int a) { (void)a; return pref_load_warn; }
static void set_pref_warn(int a, int v) { (void)a; pref_load_warn = v; storage_prefs_changed(); }
static void range_warn(int a, int *lo, int *hi) { (void)a; *lo = WARN_ALWAYS; *hi = WARN_NEVER; }
static const char *const warn_labels[] = { "ALWAYS", "DIRTY", "NEVER" };
static void fmt_warn(char *o, int v, int a) { (void)a; o[0] = 0; cat(o, warn_labels[v]); }
static const struct field f_pref_warn = { get_pref_warn, set_pref_warn, range_warn, fmt_warn, 0, L_VLIST, warn_labels, 0 };

static int get_gate_delay(int a) { (void)a; return pref_gate_delay; }
static void set_gate_delay(int a, int v)
{
    (void)a;
    pref_gate_delay = v;
    engine_set_gate_delay(v);
    storage_prefs_changed();
}
static void range_gate_delay(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = 50; }
static void fmt_gate_delay(char *o, int v, int a) { (void)a; fmt_fixed(o, v, 1, 1, 0); cat(o, " MSEC"); }
static const struct field f_gate_delay = { get_gate_delay, set_gate_delay, range_gate_delay, fmt_gate_delay, 0, L_NONE, 0, 0 };

static int get_bright(int a) { (void)a; return pref_brightness; }
static void set_bright(int a, int v)
{
    (void)a;
    pref_brightness = v;
    oled_set_brightness(v);
    storage_prefs_changed();
}
static void range_bright(int a, int *lo, int *hi) { (void)a; *lo = 1; *hi = 11; }
static void fmt_bright(char *o, int v, int a) { (void)a; fmt_int(o, v); }
static const struct field f_bright = { get_bright, set_bright, range_bright, fmt_bright, 0, L_NONE, 0, 0 };

static const uint8_t saver_min[] = { 2, 3, 5, 10, 20, 30, 60, 0 };
static const char *const saver_labels[] = { "2 MINUTES", "3 MINUTES", "5 MINUTES", "10 MINUTES",
                                            "20 MINUTES", "30 MINUTES", "60 MINUTES", "NEVER" };
#define SAVER_NEVER 7
static int get_saver(int a)
{
    (void)a;
    int i = 0;
    if (!pref_saver) {
        return SAVER_NEVER;
    }
    while (i < SAVER_NEVER - 1 && saver_min[i + 1] <= pref_saver) {
        i++;
    }
    return i;
}
static void set_saver(int a, int v)
{
    (void)a;
    pref_saver = saver_min[v];
    storage_prefs_changed();
}
static void range_saver(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = SAVER_NEVER; }
static void fmt_saver(char *o, int v, int a) { (void)a; o[0] = 0; cat(o, saver_labels[v]); }
static const struct field f_saver = { get_saver, set_saver, range_saver, fmt_saver, 0, L_VLIST, saver_labels, 0 };

static int get_auto(int ch) { return preset.ch[ch].auto_trig; }
static void set_auto(int ch, int v) { preset.ch[ch].auto_trig = (uint8_t)v; }
static const char *const auto_labels[] = { "NORMAL", "AUTO" };
static void fmt_auto(char *o, int v, int a) { (void)a; o[0] = 0; cat(o, auto_labels[v]); }
static const struct field f_auto = { get_auto, set_auto, range_onoff, fmt_auto, 0, L_VLIST, auto_labels, 0 };

/* Inc/Dec/Reset preset CV inputs of the folder; same values as D2CV. */
static int get_pcv(int i) { return engine_preset_cv_in[i]; }
static void set_pcv(int i, int v) { engine_preset_cv_in[i] = (uint8_t)v; storage_folder_cv_changed(); }
static const struct field f_pcv = { get_pcv, set_pcv, range_d2cv, fmt_d2cv, 0, L_D2CV, 0, 0 };

/* ---- Sampler channels (machine Off): zones and their samples ---- */

static int is_sampler(int ch) { return !preset.ch[ch].machine; }

static uint8_t zsel[CHANNELS];      /* selected zone per channel */

/* The Stereo/Right channel of sampler channel ch, -1 if it has none. */
static int pair_of(int ch)
{
    int r = ch + 1;

    return r < CHANNELS && is_sampler(ch) && is_sampler(r) && preset.ch[r].mode == MODE_STEREO &&
           preset.ch[ch].mode != MODE_STEREO ? r : -1;
}

static int zone_count(int ch) { return zones_count(&preset.ch[ch].s); }

static int cur_zone(void)
{
    int n = zone_count(sel);

    return zsel[sel] < n ? zsel[sel] : n ? n - 1 : 0;
}

static struct zone_cfg *zcfg(int ch, int z) { return &preset.ch[ch].s.zone[z]; }

static const struct sample *zone_smp(int ch, int z)
{
    int i = preset.ch[ch].s.zone[z].smp - 1;

    return i >= 0 && i < samples_count() ? &samples[i] : 0;
}

/* Frames of the zone's sample once its header is known, else 0. */
static uint32_t zone_frames(int ch, int z)
{
    const struct sample *m = zone_smp(ch, z);

    return m && m->state >= SMP_HEADER && m->state <= SMP_NOMEM ? m->frames : 0;
}

/* Selecting a zone moves a Zone Lock on that channel along with it. */
static void zone_select(int ch, int z)
{
    zsel[ch] = (uint8_t)z;
    if (engine_zone_lock[ch]) {
        engine_zone_lock[ch] = (uint8_t)(z + 1);
    }
}

static void irq_off(void) { __asm__ volatile("cpsid i" ::: "memory"); }
static void irq_on(void) { __asm__ volatile("cpsie i" ::: "memory"); }

/* Stereo-aware edits (stock 2.0): a zone edit on a channel whose next one
 * is its Stereo/Right side is made there too. */
enum { ZM_POS = 1, ZM_PITCH = 2, ZM_LEVEL = 4, ZM_MINV = 8 };

static void zone_mirror(int ch, int z, int what)
{
    const struct zone_cfg *a = zcfg(ch, z);
    struct zone_cfg *b;

    if (ch + 1 >= CHANNELS || preset.ch[ch + 1].mode != MODE_STEREO || !is_sampler(ch + 1) ||
        !zcfg(ch + 1, z)->smp) {
        return;
    }
    b = zcfg(ch + 1, z);
    if (what & ZM_POS) {
        uint32_t f = irq_save();
        b->start = a->start;
        b->end = a->end;
        b->loop_start = a->loop_start;
        b->loop_len = a->loop_len;
        irq_restore(f);
    }
    if (what & ZM_PITCH) {
        b->pitch = a->pitch;
    }
    if (what & ZM_LEVEL) {
        b->level = a->level;
    }
    if (what & ZM_MINV) {
        b->minv = a->minv;
    }
}

/* Sample and loop points of the selected zone, frames (loop end and length
 * in 1/32 frame like LoopLength). */
enum { PF_START, PF_END, PF_LSTART, PF_LEND };

static int64_t z_end(const struct zone_cfg *z, uint32_t fr)
{
    return z->end == POS_END || z->end > fr ? fr : z->end;
}

static int64_t z_lend32(const struct zone_cfg *z, uint32_t fr)
{
    int64_t e = z->loop_len == POS_END ? (int64_t)fr * 32 : (int64_t)z->loop_start * 32 + z->loop_len;

    return e > (int64_t)fr * 32 ? (int64_t)fr * 32 : e;
}

static int64_t z_len32(const struct zone_cfg *z, uint32_t fr)
{
    return z_lend32(z, fr) - (int64_t)z->loop_start * 32;
}

static int get_pos(int a)
{
    const struct zone_cfg *z = zcfg(sel, cur_zone());
    uint32_t fr = zone_frames(sel, cur_zone());

    switch (a) {
    case PF_START:  return (int)z->start;
    case PF_END:    return (int)z_end(z, fr);
    case PF_LSTART: return (int)z->loop_start;
    default:        return (int)((preset.ch[sel].s.loop_is_end ? z_lend32(z, fr) : z_len32(z, fr)) / 32);
    }
}

static void range_pos(int a, int *lo, int *hi)
{
    const struct zone_cfg *z = zcfg(sel, cur_zone());
    int64_t fr = zone_frames(sel, cur_zone()), l, h;
    int end_mode = preset.ch[sel].s.loop_is_end;

    switch (a) {
    case PF_START:  l = 0; h = z_end(z, (uint32_t)fr) - 1; break;
    case PF_END:    l = (int64_t)z->start + 1; h = fr; break;
    case PF_LSTART:
        l = 0;
        h = end_mode ? z_lend32(z, (uint32_t)fr) / 32 - 4 : fr - (z_len32(z, (uint32_t)fr) + 31) / 32;
        break;
    default:
        l = end_mode ? (int64_t)z->loop_start + 4 : 4;
        h = end_mode ? fr : fr - z->loop_start;
    }
    if (fr < 8 || h < l) {
        l = h = get_pos(a);
    }
    *lo = (int)l;
    *hi = (int)h;
}

static int play_held(void)
{
    return last_panel && (last_panel->held & (1u << BTN_PLAY_MODE));
}

static void set_pos(int a, int v)
{
    int zi = cur_zone();
    struct zone_cfg *z = zcfg(sel, zi);
    uint32_t fr = zone_frames(sel, zi);
    int64_t e32 = z_lend32(z, fr), l32 = z_len32(z, fr);

    irq_off();
    switch (a) {
    case PF_START:
        z->start = (uint32_t)v;
        break;
    case PF_END:
        z->end = (uint32_t)v >= fr ? POS_END : (uint32_t)v;
        break;
    case PF_LSTART:
        z->loop_start = (uint32_t)v;
        if (preset.ch[sel].s.loop_is_end) {
            z->loop_len = z->loop_len == POS_END ? POS_END : (uint32_t)(e32 - (int64_t)v * 32);
        } else {
            z->loop_len = (int64_t)v * 32 + l32 >= (int64_t)fr * 32 ? POS_END : (uint32_t)l32;
        }
        break;
    default:
        if (preset.ch[sel].s.loop_is_end) {
            z->loop_len = (uint32_t)v >= fr ? POS_END : (uint32_t)(((int64_t)v - z->loop_start) * 32);
        } else {
            int64_t l = (int64_t)v * 32 + (l32 & 31);
            z->loop_len = (int64_t)z->loop_start * 32 + l >= (int64_t)fr * 32 ? POS_END : (uint32_t)l;
        }
    }
    /* PLAY MODE held while setting the loop: the sample spans the loop (2.0). */
    if (a >= PF_LSTART && play_held()) {
        int64_t e = z_lend32(z, fr) / 32;
        z->start = z->loop_start;
        z->end = e >= fr ? POS_END : (uint32_t)e;
        play_held_used = 1;
    }
    zone_mirror(sel, zi, ZM_POS);
    irq_on();
}

static void fmt_pos(char *o, int v, int a)
{
    (void)a;
    put_uint(o, (unsigned)v, 8);
}

static int pos_zoom = 1;    /* 1..32, shared by the four point pages (2.0) */
static const struct field f_pos = { get_pos, set_pos, range_pos, fmt_pos, 0, L_NONE, 0, 0 };

/* Channel routes of a sampler page: amounts -1.00..+1.00 as stock. */
static void range_amt1(int d, int *lo, int *hi) { (void)d; *lo = -100; *hi = 100; }
static const struct field f_amt1 = { get_amt, set_amt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

static int get_ssrc(int i) { return preset.ch[sel].s.mod[i].src; }
static void set_ssrc(int i, int v) { preset.ch[sel].s.mod[i].src = (uint8_t)v; }
static const struct field f_ssrc = { get_ssrc, set_ssrc, range_src, fmt_src_field, 0, L_CVSRC, 0, 0 };
static int get_samt(int i) { return preset.ch[sel].s.mod[i].amt; }
static void set_samt(int i, int v) { preset.ch[sel].s.mod[i].amt = (int16_t)v; }
static const struct field f_samt = { get_samt, set_samt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

/* Sampler values with stock's ranges and resolutions. */
enum { SV_PITCH, SV_LEVEL, SV_ZPITCH, SV_ZLEVEL, SV_ATTACK, SV_RELEASE, SV_ALIAS, SV_PMINDEX, SV_DELAY };

static void fmt_sdelay(char *o, int t)
{
    if (t < 100) {
        fmt_fixed(o, t, 1, 1, 0);
    } else {
        fmt_int(o, t / 10);
    }
    cat(o, " MS");
}

static int get_sval(int a)
{
    const struct channel_cfg *c = &preset.ch[sel];

    switch (a) {
    case SV_PITCH:   return c->pitch;
    case SV_LEVEL:   return c->level;
    case SV_ZPITCH:  return zcfg(sel, cur_zone())->pitch;
    case SV_ZLEVEL:  return zcfg(sel, cur_zone())->level;
    case SV_ATTACK:  return env_index(c->s.attack);
    case SV_RELEASE: return env_index(c->s.release);
    case SV_PMINDEX: return c->s.pm_index;
    case SV_DELAY:   return sdelay_index(c->s.start_delay);
    default:         return c->s.aliasing;
    }
}

static void set_sval(int a, int v)
{
    struct channel_cfg *c = &preset.ch[sel];

    switch (a) {
    case SV_PITCH:   c->pitch = (int16_t)v; break;
    case SV_LEVEL:   c->level = (int16_t)v; break;
    case SV_ZPITCH:  zcfg(sel, cur_zone())->pitch = (int16_t)v; zone_mirror(sel, cur_zone(), ZM_PITCH); break;
    case SV_ZLEVEL:  zcfg(sel, cur_zone())->level = (int16_t)v; zone_mirror(sel, cur_zone(), ZM_LEVEL); break;
    case SV_ATTACK:  c->s.attack = env_time(v); break;
    case SV_RELEASE: c->s.release = env_time(v); break;
    case SV_PMINDEX: c->s.pm_index = (int16_t)v; break;
    case SV_DELAY:   c->s.start_delay = (uint16_t)sdelay_time(v); break;
    default:         c->s.aliasing = (uint8_t)v;
    }
}

static void range_sval(int a, int *lo, int *hi)
{
    switch (a) {
    case SV_PITCH:
    case SV_ZPITCH: *lo = SPITCH_MIN; *hi = SPITCH_MAX; return;
    case SV_LEVEL:
    case SV_ZLEVEL: *lo = SLEVEL_MIN; *hi = SLEVEL_MAX; return;
    case SV_ATTACK:
    case SV_RELEASE: *lo = 0; *hi = ENV_STEPS; return;
    case SV_DELAY:  *lo = 0; *hi = SDELAY_STEPS; return;
    default:        *lo = 0; *hi = 100;
    }
}

static void fmt_sval(char *o, int v, int a)
{
    uint32_t t;

    switch (a) {
    case SV_PITCH:
    case SV_ZPITCH:
        fmt_fixed(o, v, 2, 2, 1);
        cat(o, " SEMI");
        return;
    case SV_LEVEL:
    case SV_ZLEVEL:
        fmt_fixed(o, v, 1, 2, 1);
        cat(o, " DB");
        return;
    case SV_ATTACK:
    case SV_RELEASE:
        t = env_time(v);
        if (t < 100) {
            fmt_fixed(o, (int)t, 4, 1, 0);
        } else if (t < 10000) {
            fmt_fixed(o, (int)t / 10, 3, 1, 0);
        } else if (t < 100000) {
            fmt_fixed(o, (int)t / 100, 2, 1, 0);
        } else {
            fmt_fixed(o, (int)t / 1000, 1, 1, 0);
        }
        cat(o, " SEC");
        return;
    case SV_PMINDEX:
        fmt_fixed(o, v, 2, 1, 1);
        return;
    case SV_DELAY:
        fmt_sdelay(o, sdelay_time(v));
        return;
    default:
        put_uint(o, (unsigned)v, 3);
        cat(o, "%");
    }
}

static const struct field f_sval = { get_sval, set_sval, range_sval, fmt_sval, 0, L_NONE, 0, 0 };

/* Enumerated sampler settings: popup lists. */
#define CHOICE(name, expr, ...) \
    static const char *const name##_labels[] = { __VA_ARGS__ }; \
    static int get_##name(int a) { (void)a; return preset.ch[sel].expr; } \
    static void set_##name(int a, int v) { (void)a; preset.ch[sel].expr = (uint8_t)v; } \
    static void range_##name(int a, int *lo, int *hi) \
    { (void)a; *lo = 0; *hi = (int)(sizeof(name##_labels) / sizeof(name##_labels[0])) - 1; } \
    static void fmt_##name(char *o, int v, int a) { (void)a; o[0] = 0; cat(o, name##_labels[v]); } \
    static const struct field f_##name = { get_##name, set_##name, range_##name, fmt_##name, 0, L_VLIST, \
                                           name##_labels, 0 };

CHOICE(afc, s.attack_from_current, "FROM ZERO", "FROM LAST")
CHOICE(play, s.play_mode, "GATED", "1 SHOT")
CHOICE(loop, s.loop_mode, "OFF", "NORMAL", "GATED")
CHOICE(extenv, s.linam_ext_env, "NORMAL", "EXT ENV")
CHOICE(rev, s.reverse, "OFF", "ON")
CHOICE(smooth, s.splice, "OFF", "ON")
CHOICE(fader, s.mixmod_fader, "NORMAL", "FADER")
CHOICE(zrt, s.zones_rt, "GATE RISE", "CONTINUOUS", "RANDOM", "ADVANCE")
CHOICE(pmsrc, s.pm_source, "CHANNEL 1", "CHANNEL 2", "CHANNEL 3", "CHANNEL 4", "CHANNEL 5", "CHANNEL 6",
       "CHANNEL 7", "CHANNEL 8", "LEFT INPUT", "RIGHT INPUT", "SELECT CV")

static int get_zcv(int a) { (void)a; return preset.ch[sel].s.zones_cv; }
static void set_zcv(int a, int v) { (void)a; preset.ch[sel].s.zones_cv = (uint8_t)v; }
static int get_rcv(int a) { (void)a; return preset.ch[sel].s.rev_cv; }
static void set_rcv(int a, int v) { (void)a; preset.ch[sel].s.rev_cv = (uint8_t)v; }
static const struct field f_rcv = { get_rcv, set_rcv, range_src, fmt_src_field, 0, L_CVSRC, 0, 0 };
static const struct field f_zcv = { get_zcv, set_zcv, range_src, fmt_src_field, 0, L_CVSRC, 0, 0 };

/* ZONES: a zone's sample (popup of the folder's WAVs; "- -" deletes the
 * zone, the "++" row adds one) and its lowest selecting voltage. */
static void zone_sel_fix(int ch)
{
    int n = zone_count(ch);

    if (zsel[ch] >= n && zsel[ch]) {
        zone_select(ch, n ? n - 1 : 0);
    }
    if (engine_zone_lock[ch] > n) {
        engine_zone_lock[ch] = 0;
    }
}

/* Zone z of ch goes, and on its Stereo/Right side too (stock 2.0). */
static void zone_delete(int ch, int z)
{
    int r = pair_of(ch);

    irq_off();
    zones_delete(&preset.ch[ch].s, z);
    if (r >= 0) {
        zones_delete(&preset.ch[r].s, z);
    }
    irq_on();
    zone_sel_fix(ch);
    if (r >= 0) {
        zone_sel_fix(r);
    }
}

/* A new last zone takes the lower half of the range of the zone above. */
static void zone_append(int ch, const struct zone_cfg *from, int smp)
{
    struct zone_cfg z;
    int n;

    if (from) {
        z = *from;
    } else {
        zone_defaults(&z);
    }
    if (z.smp != smp + 1) {
        z.beats = 0;            /* Time Lock's Beats belongs to the sample (Tallent) */
    }
    z.smp = (uint16_t)(smp + 1);
    irq_off();
    n = zones_append(&preset.ch[ch].s, &z);
    irq_on();
    if (n >= 0) {
        zone_select(ch, n);
    }
}

/* Assigning a sample to a zone. A stereo sample on a channel whose next one
 * is its Stereo/Right side, or an empty Master (which becomes one), takes
 * both: the left plane here, the right there (stock 2.0). Changing a
 * paired mono sample follows on the Stereo/Right side. */
static int sample_stereo(int smp)
{
    return samples_valid(smp) && samples[smp].state >= SMP_HEADER && samples[smp].state <= SMP_NOMEM &&
           samples[smp].channels == 2;
}

static int pair_free(int ch)
{
    int r = ch + 1;

    return r < CHANNELS && is_sampler(r) && preset.ch[ch].mode != MODE_STEREO &&
           (preset.ch[r].mode == MODE_STEREO || (preset.ch[r].mode == MODE_MASTER && !zone_count(r)));
}

/* Sample field values: a samples[] index, with SMP_SIDE_R for the right
 * side of a stereo sample (stock lists both sides). */
#define SMP_SIDE_R 0x10000
#define SMP_INDEX(v) ((v) >= 0 ? (v) & 0xffff : (v))

static void zone_put(int ch, int z, int smp, int side, const struct zone_cfg *like)
{
    if (z >= zone_count(ch)) {
        if (z > zone_count(ch)) {
            return;
        }
        zone_append(ch, like, smp);
        z = zone_count(ch) - 1;
    }
    irq_off();
    if (zcfg(ch, z)->smp != smp + 1) {
        zcfg(ch, z)->beats = 0;
    }
    zcfg(ch, z)->smp = (uint16_t)(smp + 1);
    zcfg(ch, z)->side = (uint8_t)side;
    irq_on();
}

/* After master ch's zone z got a sample (old: its previous samples[]
 * index, -1 for a zone added to its n), its Stereo/Right side follows. */
static void sr_follow(int ch, int z, int old, int n)
{
    int r = pair_of(ch), stereo;

    if (r < 0 || z >= zone_count(ch)) {
        return;
    }
    stereo = sample_stereo(zcfg(ch, z)->smp - 1);
    irq_off();
    zones_sr_follow(&preset.ch[ch].s, &preset.ch[r].s, z, old + 1, n, stereo);
    irq_on();
}

/* side: which plane of a stereo sample plays here. L takes the next
 * channel as its Stereo/Right with R (an existing pair gets the other side
 * either way); R on an empty channel after a Master playing that sample's L
 * makes it that Master's Stereo/Right. */
static void assign_sample(int ch, int z, int smp, int side)
{
    int n = zone_count(ch), old = z < n ? zcfg(ch, z)->smp - 1 : -1, r = ch + 1, l = ch - 1;

    if (samples_valid(smp)) {
        samples_probe(smp);     /* unused WAVs are only names until picked */
    }
    side = side && sample_stereo(smp);
    z = z > n ? n : z;
    if (side && !n && l >= 0 && is_sampler(l) && preset.ch[l].mode == MODE_MASTER &&
        preset.ch[ch].mode == MODE_MASTER && zone_count(l) && zcfg(l, 0)->smp == smp + 1 && !zcfg(l, 0)->side) {
        irq_off();
        preset.ch[ch].mode = MODE_STEREO;
        irq_on();
        zone_put(ch, 0, smp, 1, zcfg(l, 0));
        return;
    }
    if (sample_stereo(smp) && pair_free(ch) && (!side || preset.ch[r].mode == MODE_STEREO) &&
        preset.ch[r].mode == MODE_MASTER) {
        irq_off();
        preset.ch[r].mode = MODE_STEREO;
        irq_on();
    }
    zone_put(ch, z, smp, side, z == n && n ? zcfg(ch, n - 1) : 0);   /* "++" copies the last zone (stock) */
    sr_follow(ch, z, old, n);
}

/* Preview: the target's channels are copied first and put back before each
 * step, so a highlight never leaves anything behind; the sample loads on
 * demand and, if nothing else uses it, is freed again (a file from outside
 * the folder is a preview entry). With Auto-Audition it plays 2.5 s once
 * loaded. */
static struct {
    int on, ch, z, cur;
    struct channel_cfg save[2];
    uint8_t lock;
} pv;
static uint8_t aud_wait, aud_mask;
static uint32_t aud_until;
#define AUDITION_US 2500000u

static void pv_begin(int ch, int z)
{
    pv.on = 1;
    pv.ch = ch;
    pv.z = z;
    pv.cur = -1;
    pv.save[0] = preset.ch[ch];
    if (ch + 1 < CHANNELS) {
        pv.save[1] = preset.ch[ch + 1];
    }
    pv.lock = engine_zone_lock[ch];
}

static void pv_restore(void)
{
    irq_off();
    preset.ch[pv.ch] = pv.save[0];
    if (pv.ch + 1 < CHANNELS) {
        preset.ch[pv.ch + 1] = pv.save[1];
    }
    irq_on();
}

static void pv_drop(void)
{
    int i = pv.cur;

    pv.cur = -1;
    if (i < 0 || storage_sample_used(i)) {
        return;
    }
    if (i >= SAMPLES_MAX) {
        storage_preview_release(i);
    } else {
        storage_sample_unload(i);
    }
}

static void pv_step(int v)
{
    int smp = SMP_INDEX(v);

    pv_restore();
    pv_drop();
    if (smp < 0) {
        return;
    }
    assign_sample(pv.ch, pv.z, smp, (v & SMP_SIDE_R) != 0);
    pv.cur = smp;
    storage_sample_demand(smp);
    if (zone_count(pv.ch) > pv.z) {
        engine_zone_lock[pv.ch] = (uint8_t)(pv.z + 1);
    }
    aud_wait = (uint8_t)pref_auto_audition;
}

/* After the last pv_restore (and the final assignment, if any). */
static void pv_end(void)
{
    pv_drop();
    engine_zone_lock[pv.ch] = pv.lock;
    pv.on = 0;
    aud_wait = 0;
}

static void aud_poll(void)
{
    uint32_t now = time_us();

    if (aud_wait) {
        int i = pv.on ? pv.cur : -1;

        if (i < 0 || samples[i].state == SMP_BAD || samples[i].state == SMP_NOMEM) {
            aud_wait = 0;
        } else if (samples[i].state == SMP_READY) {
            aud_wait = 0;
            aud_mask = (uint8_t)(1u << pv.ch);
            aud_until = now + AUDITION_US;
            engine_virtual_buttons = aud_mask;
            engine_trigger(pv.ch);
        }
    }
    if (aud_mask && (int32_t)(now - aud_until) >= 0) {
        aud_mask = 0;
        engine_virtual_buttons = 0;
    }
}

/* ZONES/CHANNELS sample field; arg = channel * ZONES + zone. -1 deletes the
 * zone, the "++" row adds one, -3 (Channels page only) is the machine list. */
static int get_zsmp(int a)
{
    const struct zone_cfg *zc = zcfg(a / ZONES, a % ZONES);

    if (a % ZONES >= zone_count(a / ZONES)) {
        return -2;
    }
    return (zc->smp - 1) | (zc->side && sample_stereo(zc->smp - 1) ? SMP_SIDE_R : 0);
}
static void set_zsmp(int a, int v)
{
    int ch = a / ZONES, z = a % ZONES, side = v >= 0 && (v & SMP_SIDE_R);

    v = SMP_INDEX(v);
    if (v == -1) {
        zone_delete(ch, z);
        if (page == P_ZONES) {
            focus[P_ZONES] = 2 * (z < zone_count(ch) ? z : z ? z - 1 : 0);
        }
    } else if (v >= 0) {
        int append = z >= zone_count(ch);

        assign_sample(ch, z, v, side);
        storage_sample_demand(v);
        if (append && page == P_ZONES) {
            focus[P_ZONES] = 2 * z;
        }
    }
}
static void range_zsmp(int a, int *lo, int *hi)
{
    *lo = a % ZONES < zone_count(a / ZONES) ? -1 : 0;
    *hi = samples_count() - 1;
}
static void cat_side(char *o, int v)
{
    if (v >= 0 && sample_stereo(SMP_INDEX(v))) {
        cat(o, v & SMP_SIDE_R ? FONT_SUB_R : FONT_SUB_L);
    }
}
static void fmt_zsmp(char *o, int v, int a)
{
    (void)a;
    o[0] = 0;
    if (v >= 0 && samples_valid(SMP_INDEX(v))) {
        copy_n(o, samples[SMP_INDEX(v)].name, 20);
        cat_side(o, v);
    } else {
        cat(o, v == -1 ? "- -" : v == -3 ? "../" : "++");
    }
}
static void fmt_chsmp(char *o, int v, int a)
{
    char *dot;

    (void)a;
    o[0] = 0;
    if (v < 0 || !samples_valid(SMP_INDEX(v))) {
        cat(o, v == -3 ? "../" : v == -1 ? "- -" : "OFF");
        return;
    }
    copy_n(o, samples[SMP_INDEX(v)].name, 8);
    if ((dot = strchr(o, '.'))) {
        *dot = 0;
    }
    cat_side(o, v);
}
static void hold_zsmp(int a);
static void hold_machine(int ch);
static const struct field f_zsmp = { get_zsmp, set_zsmp, range_zsmp, fmt_zsmp, 0, L_SAMPLE, 0, hold_zsmp };
static const struct field f_chsmp = { get_zsmp, set_zsmp, range_zsmp, fmt_chsmp, 0, L_SAMPLE, 0, hold_machine };

static int get_zvolt(int z) { return zcfg(sel, z)->minv; }
static void set_zvolt(int z, int v)
{
    zcfg(sel, z)->minv = (int16_t)v;
    zone_mirror(sel, z, ZM_MINV);
}
/* Between the boundaries above and below (stock). */
static void range_zvolt(int z, int *lo, int *hi)
{
    int n = zone_count(sel);

    *hi = z > 0 ? zcfg(sel, z - 1)->minv : 500;
    *lo = z + 2 < n ? zcfg(sel, z + 1)->minv : -500;
}
static void fmt_zvolt(char *o, int v, int z) { (void)z; fmt_fixed(o, v, 2, 1, 1); }

/* Equal ranges over -5..+5 V. */
static void hold_zvolt(int z)
{
    int n = zone_count(sel);

    (void)z;
    irq_off();
    for (int k = 0; k < n; k++) {
        zcfg(sel, k)->minv = (int16_t)(k + 1 < n ? 500 - ((k + 1) * 2000 + n) / (2 * n) : -500);
        zone_mirror(sel, k, ZM_MINV);
    }
    irq_on();
    ui_say("RANGES EQUALISED");
}
static const struct field f_zvolt = { get_zvolt, set_zvolt, range_zvolt, fmt_zvolt, 0, L_NONE, 0, hold_zvolt };

static void open_page(int p);
static void bench_start(int a);
static const struct field f_bench = { 0, 0, 0, 0, bench_start, L_NONE, 0, 0 };
static void act_raw(int a) { (void)a; cv_raw_view = !cv_raw_view; }
static const struct field f_raw = { 0, 0, 0, 0, act_raw, L_NONE, 0, 0 };
static void act_open(int p) { open_page(p); }
static const struct field f_open = { 0, 0, 0, 0, act_open, L_NONE, 0, 0 };

static void act_init(int a)
{
    (void)a;
    if (!init_armed) {
        init_armed = 1;
        return;
    }
    __asm__ volatile("cpsid i" ::: "memory");
    preset_init(&preset);
    __asm__ volatile("cpsie i" ::: "memory");
    init_armed = 0;
    ui_say("PRESET INITIALIZED");
    open_page(P_HOME);
}
static const struct field f_init = { 0, 0, 0, 0, act_init, L_NONE, 0, 0 };

static void say_error(const char *fallback)
{
    ui_say(storage_error ? storage_error : fallback);
}

/* LOAD page: a folder list; click loads (after the pref's warning),
 * double click imports from it (load_click). */
static int load_pick = -1;
static void folder_menu(void);

/* A folder to create just before loading (after the unsaved-changes
 * warning). */
static char new_name[64];
static int new_pending;

static void load_folder_now(void)
{
    char msg[24];
    const char *name = storage_folder_name(load_pick);

    if (new_pending) {
        new_pending = 0;
        if (!storage_new_folder(new_name)) {
            say_error("CAN'T CREATE");
            return;
        }
        name = new_name;
    }
    if (storage_load_folder(name, 0)) {
        copy_n(msg, storage_folder(), 20);
        ui_say(msg);
        open_page(P_HOME);
    } else {
        say_error("LOAD FAILED");
    }
}

static void load_and_warn(void);

static void act_folder(int i)
{
    load_pick = i;
    new_pending = 0;
    load_and_warn();
}
static void load_click(int row);
static const struct field f_folder = { 0, 0, 0, 0, load_click, L_NONE, 0, 0 };

static void load_and_warn(void)
{
    int dirty = storage_changed_count();

    if (pref_load_warn == WARN_ALWAYS || (pref_load_warn == WARN_DIRTY && dirty)) {
        folder_menu();
    } else {
        load_folder_now();
    }
}

/* SAVE TO page: target slot (presets in the folder, or the free slot New)
 * and the name; click on SAVE writes. */
static int save_slot, save_new, save_armed;

static int save_candidate(int n)
{
    return n == save_new || storage_state(n) != SLOT_EMPTY;
}

static int get_save_slot(int a) { (void)a; return save_slot; }
/* Snaps to the nearest candidate in the direction of the change. */
static void set_save_slot(int a, int v)
{
    int dir = v > save_slot ? 1 : -1;

    (void)a;
    for (int n = v; n >= 1 && n <= PRESET_SLOTS; n += dir) {
        if (save_candidate(n)) {
            save_slot = n;
            break;
        }
    }
    save_armed = 0;
}
static void range_save_slot(int a, int *lo, int *hi) { (void)a; *lo = 1; *hi = PRESET_SLOTS; }
static void fmt_save_slot(char *o, int v, int a)
{
    (void)a;
    put_uint(o, (unsigned)v, 3);
    if (v == save_new) {
        cat(o, " NEW");
    }
}
static const struct field f_save_slot = { get_save_slot, set_save_slot, range_save_slot, fmt_save_slot, 0, L_NONE, 0, 0 };

static void act_save_to(int a)
{
    char msg[24] = "SAVED TO ";

    (void)a;
    if (!storage_card()) {
        ui_say("NO CARD: NOT SAVED");
        return;
    }
    if (save_slot != preset_number && save_slot != save_new && !save_armed) {
        save_armed = 1;
        return;
    }
    if (storage_save(save_slot)) {
        put_uint(msg + 9, (unsigned)save_slot, 3);
        ui_say(msg);
        open_page(P_HOME);
    } else {
        say_error("SAVE FAILED");
    }
    save_armed = 0;
}
static const struct field f_save_go = { 0, 0, 0, 0, act_save_to, L_NONE, 0, 0 };

static const char name_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_#.";
#define NAME_CHARS ((int)sizeof(name_chars) - 1)

/* Lowercase (stock names) shows as uppercase; it stays lowercase unless edited. */
static int name_get(const char *buf, int i)
{
    int c = buf[i] >= 'a' && buf[i] <= 'z' ? buf[i] - 32 : buf[i];

    for (int k = 0; k < NAME_CHARS; k++) {
        if (name_chars[k] == c) {
            return k;
        }
    }
    return 0;
}

static void name_set(char *buf, int len, int i, int v)
{
    buf[i] = name_chars[v];
    int end = len;
    while (end > 0 && (buf[end - 1] == ' ' || buf[end - 1] == 0)) {
        end--;
    }
    for (int k = 0; k < len; k++) {
        if (k >= end) {
            buf[k] = 0;
        } else if (!buf[k]) {
            buf[k] = ' ';
        }
    }
}

static int get_name(int i) { return name_get(preset.name, i); }
static void set_name(int i, int v) { name_set(preset.name, PRESET_NAME_LEN, i, v); }
static void range_name(int i, int *lo, int *hi) { (void)i; *lo = 0; *hi = NAME_CHARS - 1; }
static void fmt_name(char *o, int v, int i) { (void)i; o[0] = name_chars[v]; o[1] = 0; }
static const struct field f_name = { get_name, set_name, range_name, fmt_name, 0, L_NONE, 0, 0 };

/* Folder name editor (NEW FOLDER, RENAME, DUPLICATE): the same characters,
 * two rows. Characters outside the set that came with the old name show as
 * they are until edited. */
#define FNAME_LEN 28
#define FNAME_ROW 14
enum { FN_NEW, FN_RENAME, FN_DUP, FN_SAMPLE };

static char fname[FNAME_LEN + 1];
static char fn_src[256];
static int fn_mode;

static int get_fname(int i) { return name_get(fname, i); }
static void set_fname(int i, int v) { name_set(fname, FNAME_LEN, i, v); }
static void fmt_fname(char *o, int v, int i)
{
    int c = fname[i] >= 'a' && fname[i] <= 'z' ? fname[i] - 32 : fname[i];

    o[0] = c ? (char)c : name_chars[v];
    o[1] = 0;
}
static const struct field f_fname = { get_fname, set_fname, range_name, fmt_fname, 0, L_NONE, 0, 0 };

/* fname without surrounding spaces and trailing dots. */
static int fname_trimmed(char *o)
{
    int a = 0, b = FNAME_LEN;

    while (a < b && (fname[a] == ' ' || !fname[a])) {
        a++;
    }
    while (b > a && (fname[b - 1] == ' ' || fname[b - 1] == '.' || !fname[b - 1])) {
        b--;
    }
    copy_n(o, fname + a, b - a);
    return b - a;
}

static void futil_focus(const char *name, int fallback);
static int sop_idx, sop_ret;
static char sop_ext[8];
static int prog_dup, prog_idx;
static char prog_name[256];

static void act_fname_go(int a)
{
    char name[FNAME_LEN + 1];

    (void)a;
    if (!storage_card()) {
        ui_say("NO CARD");
        return;
    }
    fname_trimmed(name);
    if (fn_mode == FN_NEW) {
        if (!storage_name_ok(name)) {
            say_error("BAD NAME");
            return;
        }
        copy_n(new_name, name, 63);
        new_pending = 1;
        load_and_warn();
    } else if (fn_mode == FN_SAMPLE) {
        char nm[FNAME_LEN + 8];
        int n = fname_trimmed(nm);

        if (!n) {
            ui_say("NAME EMPTY");
            return;
        }
        cat(nm, sop_ext);
        if (storage_sample_rename(sop_idx, nm)) {
            ui_say("RENAMED");
            open_page(sop_ret);
        } else {
            say_error("RENAME FAILED");
        }
    } else if (fn_mode == FN_RENAME) {
        if (storage_rename_folder(fn_src, name)) {
            ui_say("RENAMED");
            open_page(P_FUTIL);
            futil_focus(name, 0);
        } else {
            say_error("RENAME FAILED");
        }
    } else if (storage_dup_start(fn_src, name)) {
        prog_dup = 1;
        prog_idx = focus[P_FUTIL];
        copy_n(prog_name, name, 255);
        open_page(P_PROGRESS);
    } else {
        say_error("DUPLICATE FAILED");
    }
}
static const struct field f_fname_go = { 0, 0, 0, 0, act_fname_go, L_NONE, 0, 0 };

static void fname_open(int mode, const char *src)
{
    fn_mode = mode;
    copy_n(fn_src, src, 255);
    memset(fname, 0, sizeof(fname));
    if (mode != FN_NEW) {
        copy_n(fname, src, FNAME_LEN);
    }
    open_page(P_FNAME);
}

/* FOLDER UTILITIES: pick a folder, then a menu of what to do with it. */
static char fu_name[256];
static void futil_menu(void);

static void act_futil_folder(int i)
{
    copy_n(fu_name, storage_folder_name(i), 255);
    futil_menu();
}
static const struct field f_futil_folder = { 0, 0, 0, 0, act_futil_folder, L_NONE, 0, 0 };

static void act_ufolder(int a)
{
    if (!storage_card()) {
        ui_say("NO CARD");
    } else if (a == 0) {
        fname_open(FN_NEW, "");
    } else {
        open_page(P_FUTIL);
    }
}
static const struct field f_ufolder = { 0, 0, 0, 0, act_ufolder, L_NONE, 0, 0 };

/* Registers a field without drawing it; returns 1 if focused. */
static int field_reg(const struct field *f, int arg)
{
    int idx = nfields < MAX_FIELDS ? nfields++ : MAX_FIELDS - 1;

    fields[idx].f = f;
    fields[idx].arg = arg;
    return idx == focus[page];
}

static int field_dim;   /* draw unfocused fields grayed out (inactive) */

/* Registers a field and draws its value (right-aligned at `right` if
 * nonzero); returns 1 if focused. */
static int field(int x, int y, const struct field *f, int arg, int right)
{
    const struct field *o = rofs_sub(f, &arg);
    int focused = field_reg(o, arg);
    char s[24];

    o->fmt(s, o->get(arg), arg);
    if (right) {
        x = right - gfx_text_width(s) + 1;
    }
    if (focused) {
        gfx_text_box(x, y, s, C_BLACK, fine ? C_ORANGE : C_WHITE);
    } else {
        gfx_text(x, y, s, field_dim ? C_DIM : C_WHITE);
    }
    if (o != f) {
        rofs_mark(x + gfx_text_width(s) + 2, y + 1, arg);
    }
    return focused;
}

/* ---- Input ---- */

static void card_check(void)
{
    if (storage_refresh()) {
        ui_say("CARD MOUNTED");
    }
}

static void imp_cancel_if_browsing(void);

static void open_page(int p)
{
    if (page == P_IMPORT && p != P_IMPORT) {
        imp_cancel_if_browsing();
    }
    if (page == P_CAL && p != P_CAL) {
        cal_page(0);
    }
    if (p == P_CAL && page != P_CAL) {
        cal_page(1);
        cal_msg = cal_msg2 = 0;
    }
    page = p;
    fine = 0;
    if (p == P_UTIL) {
        card_check();
    }
    if (p == P_SOFTWARE) {
        update_clear();
    }
    if (p == P_SDCARD) {
        storage_scan();
        storage_usage(&sd_total_kb, &sd_free_kb);
    }
    if (p == P_FNAME) {
        focus[P_FNAME] = 0;
        fine = 1;
    }
    if (p == P_LOAD || p == P_FUTIL) {
        card_check();
        storage_scan();
        focus[p] = 0;
        for (int i = 0; i < storage_folders(); i++) {
            if (!strcmp_(storage_folder_name(i), storage_folder())) {
                focus[p] = i;
            }
        }
    }
    if (p == P_SAVE) {
        save_new = storage_new_slot();
        save_slot = preset_number;
        save_armed = 0;
        focus[P_SAVE] = 0;
        fine = 1;
    }
    d2_engaged = 0;
    d2_ref = d2_now;
    init_armed = 0;
}

static void futil_focus(const char *name, int fallback)
{
    int n = storage_folders();

    focus[P_FUTIL] = fallback < n ? fallback : n - 1;
    for (int i = 0; i < n; i++) {
        if (!strcmp_(storage_folder_name(i), name)) {
            focus[P_FUTIL] = i;
        }
    }
    if (focus[P_FUTIL] < 0) {
        focus[P_FUTIL] = 0;
    }
}

static void focus_moved(void)
{
    fine = 0;
    d2_engaged = 0;
    d2_ref = d2_now;
    init_armed = 0;
}

static void edit(const struct field_ref *r, int v)
{
    int lo, hi;

    r->f->range(r->arg, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    if (v != r->f->get(r->arg)) {
        r->f->set(r->arg, v);
    }
}

static void page_button(int target)
{
    open_page(page == target ? P_HOME : target);
}

static void sampler_only(const char *title)
{
    if (page == P_SAMPLER_ONLY && sampler_only_title == title) {
        open_page(P_HOME);
        return;
    }
    sampler_only_title = title;
    open_page(P_SAMPLER_ONLY);
}

/* ---- Popup lists: click on a list field opens one, DATA1 scrolls, click
 * picks, any other button cancels. Channel buttons only trigger. ---- */

#define POP_MAX (PRESET_SLOTS + 1)
#define POP_PREVIEW_US 150000u

struct pop_item {
    int16_t v;
    uint8_t x, y;
};

static struct {
    int open, orig, idx, n;
    int mlevel, mfam;           /* machine picker: 1 families, 2 a family's models */
    int nsmp;                   /* the last nsmp items are pop_smp[] (sample lists) */
    int browse;                 /* 1 presets, 2 channels of another folder's preset */
    int pv_due;
    uint32_t moved_at;
    const char *title;
    struct field_ref ref;
    struct channel_cfg saved;
    struct pop_item item[POP_MAX];
} pop;
static int32_t pop_smp[2 * SAMPLES_MAX];
/* The channel as it was before SAMPLE was picked in the machine picker:
 * cancelling the sample list puts it back, only a pick commits the switch. */
static struct channel_cfg pop_origin;
static int pop_origin_set;

static int pop_v(int i)
{
    int k = i - (pop.n - pop.nsmp);

    return k >= 0 ? pop_smp[k] : pop.item[i].v;
}

static void pop_add(int v, int x, int y)
{
    if (pop.n < POP_MAX && !pop.nsmp) {
        pop.item[pop.n].v = (int16_t)v;
        pop.item[pop.n].x = (uint8_t)x;
        pop.item[pop.n].y = (uint8_t)y;
        pop.n++;
    }
}

/* Inputs 1A..8C as the panel jacks: a row per channel. */
static void pop_cv_grid(int first)
{
    for (int i = 0; i < CV_INPUTS; i++) {
        pop_add(first + i, 20 + (i % 3) * 36, 29 + (i / 3) * 11);
    }
}

static void pop_fresh(void)
{
    pop.pv_due = 0;
    pop.moved_at = time_us();
    d2_engaged = 0;
    d2_ref = d2_now;
}

static void rv_save(void);
static void rv_restore(void);
static void rv_label(char *s, int v);
static int rv_has_programs(void);

#define POP_OFF (-6)

static int chan_off(int ch) { return !preset.ch[ch].machine && !zone_count(ch); }

/* No machine and no zones. */
static void chan_set_off(int ch)
{
    irq_off();
    preset_set_machine(&preset.ch[ch], 0);
    for (int z = 0; z < ZONES; z++) {
        zone_defaults(&preset.ch[ch].s.zone[z]);
    }
    irq_on();
    zsel[ch] = 0;
    engine_zone_lock[ch] = 0;
}

/* Level 1: OFF, SAMPLE (0), the families (1 + family); level 2: "../"
 * (-3), the family's machines. */
static void mpop_fill(int level, int fam)
{
    int cur = preset.ch[pop.ref.arg].machine, curfam = cur > 0 ? voices[cur]->family : -1;

    pop.n = 0;
    pop.mlevel = level;
    pop.mfam = fam;
    pop.idx = 0;
    if (level == 1) {
        pop.orig = chan_off(pop.ref.arg) ? POP_OFF : curfam + 1;
        pop_add(POP_OFF, 0, 0);
        for (int i = 0; i <= FAMILY_COUNT; i++) {
            pop_add(i, 0, 0);
        }
        pop.idx = fam < 0 && pop.orig == POP_OFF ? 0 : fam + 2;
    } else {
        pop.orig = cur;
        pop_add(-3, 0, 0);
        for (int i = 0; i < families[fam].count; i++) {
            pop_add(families[fam].ids[i], 0, 0);
            if (families[fam].ids[i] == cur) {
                pop.idx = pop.n - 1;
            }
        }
        if (!pop.idx) {
            pop.idx = 1;
        }
    }
    pop.open = 1;
}

static void popup_open(const struct field_ref *r)
{
    int lo, hi, cur = r->f->get(r->arg);

    pop.ref = *r;
    pop.n = 0;
    pop.nsmp = 0;
    pop.browse = 0;
    pop.orig = cur;
    pop.title = 0;
    pop_fresh();
    switch (r->f->list) {
    case L_MACHINE:
        pop.saved = preset.ch[r->arg];
        if (cur > 0) {
            mpop_fill(2, voices[cur]->family);
        } else {
            mpop_fill(1, -1);
        }
        return;
    case L_REVERB:
    case L_RVPROG:
        if (r->f->list == L_RVPROG && !rv_has_programs()) {
            return;
        }
        rv_save();
        pop.mlevel = 2;
        r->f->range(r->arg, &lo, &hi);
        for (int v = lo; v <= hi; v++) {
            pop_add(v, 0, 0);
        }
        break;
    case L_CVSRC:
        for (int i = 0; i < CV_ABS; i++) {
            pop_add(i, 4 + i * 31, 14);
        }
        pop_cv_grid(CV_ABS);
        break;
    case L_D2CV:
        pop_add(0, 4, 14);
        pop_cv_grid(1);
        break;
    case L_SAMPLE:
        r->f->range(r->arg, &lo, &hi);
        if (r->f == &f_chsmp) {
            pop_add(-3, 0, 0);
        }
        if (lo < 0) {
            pop_add(-1, 0, 0);
        }
        for (int k = 0; k < samples_sorted_count(); k++) {
            int i = samples_sorted(k);

            if (i <= hi && (samples[i].state != SMP_MISSING || SMP_INDEX(cur) == i)) {
                pop_smp[pop.nsmp++] = i;
                if (sample_stereo(i)) {
                    pop_smp[pop.nsmp++] = i | SMP_SIDE_R;
                }
            }
        }
        pop.n += pop.nsmp;
        pv_begin(r->arg / ZONES, r->arg % ZONES);
        break;
    default:
        r->f->range(r->arg, &lo, &hi);
        for (int v = lo; v <= hi; v++) {
            pop_add(v, 0, 0);
        }
    }
    pop.idx = 0;
    for (int i = 0; i < pop.n; i++) {
        if (pop_v(i) == cur) {
            pop.idx = i;
        }
    }
    if (!pop.n) {
        ui_say("NO SAMPLES");
        return;
    }
    pop.open = 1;
}

static void popup_restore(void)
{
    if (pop.ref.f->list == L_MACHINE) {
        __asm__ volatile("cpsid i" ::: "memory");
        preset.ch[pop.ref.arg] = pop.saved;
        __asm__ volatile("cpsie i" ::: "memory");
    } else if (pop.ref.f->list == L_REVERB || pop.ref.f->list == L_RVPROG) {
        rv_restore();
    } else if (pop.ref.f->list == L_SAMPLE) {
        pv_restore();
        if (pop_origin_set) {
            __asm__ volatile("cpsid i" ::: "memory");
            preset.ch[pop.ref.arg / ZONES] = pop_origin;
            __asm__ volatile("cpsie i" ::: "memory");
        }
    }
}

/* Applied at once on each step; the engine only ever prepares the machine
 * the preset names now, so a fast scroll does not queue voice inits. */
static void popup_preview(void)
{
    int v = pop_v(pop.idx);

    if (!pref_browse_preview) {
        return;
    }
    if (pop.ref.f->list == L_SAMPLE) {
        pv_step(v);
        return;
    }
    if ((pop.ref.f->list != L_MACHINE && pop.ref.f->list != L_REVERB && pop.ref.f->list != L_RVPROG) || pop.mlevel != 2 || v < 0) {
        return;
    }
    if (v == pop.orig) {
        popup_restore();    /* back to the original controls, not defaults */
    } else {
        edit(&pop.ref, v);
    }
    if (pref_auto_audition && pop.ref.f->list == L_MACHINE) {
        /* The engine holds the trigger until the new voice is prepared. */
        aud_mask = (uint8_t)(1u << pop.ref.arg);
        aud_until = time_us() + AUDITION_US;
        engine_virtual_buttons = aud_mask;
        engine_trigger(pop.ref.arg);
    }
}

/* A WAV's header is read when the list first reaches it; a stereo one then
 * gets its R entry (landing on R when moving up). */
static void pop_sides(int from)
{
    int k = pop.idx - (pop.n - pop.nsmp), v;

    if (pop.ref.f->list != L_SAMPLE || k < 0 || (pop_smp[k] & SMP_SIDE_R)) {
        return;
    }
    v = pop_smp[k];
    if (samples[v].state == SMP_FOUND) {
        samples_probe(v);
    }
    if (!sample_stereo(v) || (k + 1 < pop.nsmp && pop_smp[k + 1] == (v | SMP_SIDE_R)) ||
        pop.nsmp >= 2 * SAMPLES_MAX) {
        return;
    }
    memmove(&pop_smp[k + 2], &pop_smp[k + 1], (size_t)(pop.nsmp - k - 1) * sizeof(pop_smp[0]));
    pop_smp[k + 1] = v | SMP_SIDE_R;
    pop.nsmp++;
    pop.n++;
    if (from > pop.idx) {
        pop.idx++;
    }
}

static void pop_move(int n)
{
    if (pop.n <= 0) {
        return;
    }
    n = n < 0 ? 0 : n >= pop.n ? pop.n - 1 : n;
    if (n != pop.idx) {
        int from = pop.idx;
        pop.idx = n;
        if (pop_v(n) == -4) {
            pop.idx = n > from ? n + 1 : n - 1;
            if (pop.idx < 0 || pop.idx >= pop.n) {
                pop.idx = from;
            }
        }
        pop.moved_at = time_us();
        pop_sides(from);
    }
}

static void popup_close(int pick)
{
    int v = pop.n > 0 ? pop_v(pop.idx) : pop.orig, sample = pop.ref.f->list == L_SAMPLE;

    pop.open = 0;
    if (pop.n <= 0) {
        pick = 0;
    }
    if (sample) {
        pv_restore();
    }
    if (!pick || (v == pop.orig && !sample)) {
        popup_restore();
    } else if (sample && v >= 0) {
        if (v != pop.ref.f->get(pop.ref.arg)) {
            pop.ref.f->set(pop.ref.arg, v);
        }
    } else if (v != -3) {
        edit(&pop.ref, v);
    }
    if (sample) {
        pv_end();
        pop_origin_set = 0;
    } else if (pop.ref.f->list == L_MACHINE && aud_mask) {
        aud_mask = 0;
        engine_virtual_buttons = 0;
    }
    fine = 0;
    d2_engaged = 0;
    d2_ref = d2_now;
}

/* ---- Menus: popup lists of actions (SAVE, the folder-load warning).
 * Erase and reverts take a second click. ---- */

enum { M_SAVE, M_SAVE_ALL, M_SAVE_TO, M_REVERT, M_REVERT_CH, M_ERASE, M_CANCEL, M_LOAD, M_SAVE_LOAD,
       M_RENAME, M_DUP, M_FDELETE, M_COPY_CH, M_IMP_PRESET, M_IMP_CHS, M_IMP_CH, M_SOP_RENAME, M_SOP_ERASE,
       M_IMP_NOW, M_IMP_ADVANCE, M_OP_EXPLODE, M_OP_CHOP, M_OP_BAL, M_OP_FLIP, M_OP_INSERT, M_OP_DELETE,
       M_TR_GO, M_REC_OK, M_REC_ERASE, M_INSTALL, M_CAL_ZERO, M_CAL_GAIN, M_CAL_SAVE, M_CAL_SAVE_EXIT,
       M_CAL_DISCARD, M_HANG };
static int cal_menu_input;
static void cal_do_save(int leave);
static void rec_keep_pick(int erase);

/* Channel Ops / Zone Ops menus: ops_ch is the (left) channel they act on,
 * ops_z the zone. DATA2 sets the number of the highlighted item. */
enum { BAL_5V, BAL_10V, BAL_KBD, BAL_MAJ };
static int ops_zone, ops_ch, ops_z, ops_eng, ops_ref, ops_active;
static uint8_t ops_val[4] = { 4, 4, BAL_10V, 2 };    /* by v - M_OP_EXPLODE */

/* The most an item can take: channels from ops_ch (a stereo pair takes two),
 * zones from ops_z. */
static int ops_max(int v)
{
    switch (v) {
    case M_OP_EXPLODE:
    case M_OP_CHOP:
        return ops_zone ? ZONES - ops_z : (CHANNELS - ops_ch) / (pair_of(ops_ch) >= 0 ? 2 : 1);
    case M_OP_FLIP:
        return zone_count(ops_ch) - ops_z;
    default:
        return BAL_MAJ;
    }
}

/* The item's number as shown and used; 0 when the item can't run. */
static int ops_n(int v)
{
    int n = ops_val[v - M_OP_EXPLODE], hi = ops_max(v);

    if (v == M_OP_BAL) {
        return n;
    }
    return hi < 2 ? 0 : n < 2 ? 2 : n > hi ? hi : n;
}

static uint32_t sop_at;
static int sop_down, sop_used;
static int menu_armed = -1;
static char menu_title[24];

static int get_menu(int a) { (void)a; return -1; }
static void fmt_menu(char *o, int v, int a)
{
    static const char *const names[] = {
        "SAVE PRESET", "SAVE ALL ", "SAVE TO...", "REVERT PRESET", "REVERT CHANNEL ",
        "ERASE PRESET", "CANCEL", "LOAD FOLDER", "SAVE ALL, LOAD", "RENAME", "DUPLICATE", "DELETE",
        "COPY CHANNEL", "IMPORT PRESET", "CHANNELS...", "IMPORT CHANNEL", "RENAME SAMPLE", "ERASE SAMPLE",
        "IMPORT", "LOAD AND ADVANCE", "EXPLODE ", "CHOP ", "BALANCE ", "FLIP ", "INSERT", "DELETE", "TRUNCATE",
        "OK", "ERASE", "INSTALL", "0 V CAPTURE", "+-4 V CAPTURE", "SAVE", "SAVE", "DISCARD",
        "HANG"
    };
    static const char *const bal[] = { "5V", "10V", "KBD", "MAJ" };

    (void)a;
    o[0] = 0;
    if (v == menu_armed) {
        cat(o, v == M_ERASE || v == M_SOP_ERASE || v == M_REC_ERASE ? "ERASE? CLICK AGAIN" : v == M_FDELETE ? "DELETE? CLICK AGAIN" :
                  "REVERT? CLICK AGAIN");
        return;
    }
    cat(o, names[v]);
    if (v == M_SAVE_ALL) {
        put_uint(o + 9, (unsigned)storage_changed_count(), 1);
        cat(o, " CHANGED");
    } else if (v == M_REVERT_CH) {
        char n[2] = { (char)('1' + sel), 0 };
        cat(o, n);
    } else if (v == M_OP_BAL) {
        cat(o, bal[ops_n(v)]);
    } else if (v == M_OP_EXPLODE || v == M_OP_CHOP || v == M_OP_FLIP) {
        if (ops_n(v)) {
            put_uint(o + strlen(o), (unsigned)ops_n(v), 1);
        } else {
            cat(o, "-");
        }
    }
}
static const struct field f_menu = { get_menu, 0, 0, fmt_menu, 0, L_MENU, 0, 0 };

static void menu_open(const char *title, const int *items, int n)
{
    pop.ref.f = &f_menu;
    pop.ref.arg = 0;
    pop.n = 0;
    pop.nsmp = 0;
    pop.browse = 0;
    pop.orig = -1;
    pop.idx = 0;
    pop.title = title;
    pop_fresh();
    for (int i = 0; i < n; i++) {
        pop_add(items[i], 0, 0);
    }
    menu_armed = -1;
    pop.open = 1;
}

/* ---- Pick lists: a popup over a long list (presets, channels); a click
 * calls pick_done with the item's value. ---- */

static void (*pick_label)(char *o, int v);
static void (*pick_done)(int v);

static void fmt_pick(char *o, int v, int a) { (void)a; pick_label(o, v); }
static const struct field f_pick = { get_menu, 0, 0, fmt_pick, 0, L_PICK, 0, 0 };

static void pick_begin(const char *title, void (*label)(char *, int), void (*done)(int), int browse)
{
    pop.ref.f = &f_pick;
    pop.ref.arg = 0;
    pop.n = 0;
    pop.nsmp = 0;
    pop.browse = browse;
    pop.orig = -1;
    pop.idx = 0;
    pop.title = title;
    pop_fresh();
    pick_label = label;
    pick_done = done;
    menu_armed = -1;
}

static void pick_show(int cur)
{
    pop.orig = cur;
    for (int i = 0; i < pop.n; i++) {
        if (pop_v(i) == cur) {
            pop.idx = i;
        }
    }
    pop.open = 1;
}

/* A channel written whole, with its EQ and filter (its own and its zones')
 * and, from mix (its preset's MixFX) at channel from, its compressor and
 * sends, as Tallent's copies carry them: relative CV sources (CV A/B/C)
 * stay relative, so they follow the channel to its new place. Channel 1
 * can only be a Master. */
static void put_channel(int dst, const struct channel_cfg *src, const struct mix_settings *mix, int from)
{
    struct channel_cfg c = *src;
    struct mix_channel m;

    if (dst == 0) {
        c.mode = MODE_MASTER;
    }
    mix_channel_get(mix, (u32)from, &m);
    __asm__ volatile("cpsid i" ::: "memory");
    preset.ch[dst] = c;
    __asm__ volatile("cpsie i" ::: "memory");
    mix_channel_put((u32)dst, &m);
}

static const char *slot_label(int n);

static void slot_pick_label(char *o, int v)
{
    put_uint(o, (unsigned)v, 3);
    cat(o, " ");
    cat(o, slot_label(v));
}

/* COPY CHANNEL: long-press DATA1 on the destination's machine field. */
static int copy_dst, copy_src;

/* "n NAME": the machine, else the first zone's sample (sample(ch, z), ""
 * if none), else OFF. */
static void chan_pick_label(char *o, int ch, const struct channel_cfg *c, const char *(*sample)(int, int))
{
    o[0] = (char)('1' + ch);
    o[1] = ' ';
    o[2] = 0;
    for (int z = 0; c && !c->machine && z < ZONES; z++) {
        const char *n = sample(ch, z);
        if (n[0]) {
            cat(o, n);
            return;
        }
    }
    cat(o, machine_name(c ? c->machine : 0));
}

static const char *copy_sample(int ch, int z)
{
    int i = storage_sample(copy_src, ch, z);

    return samples_valid(i) ? samples[i].name : "";
}

static void copy_chan_label(char *o, int v)
{
    const struct preset *p = storage_peek(copy_src);

    chan_pick_label(o, v, p ? &p->ch[v] : 0, copy_sample);
}

static void copy_chan_done(int ch)
{
    const struct preset *p = storage_peek(copy_src);

    if (p) {
        put_channel(copy_dst, &p->ch[ch], mix_of(p->mixfx), ch);
        ui_say("CHANNEL COPIED");
    } else {
        ui_say("CAN'T COPY");
    }
}

static void copy_preset_done(int slot)
{
    copy_src = slot;
    if (!storage_peek(slot)) {
        ui_say("CAN'T COPY");
        return;
    }
    pick_begin("COPY FROM CHANNEL", copy_chan_label, copy_chan_done, 0);
    for (int ch = 0; ch < CHANNELS; ch++) {
        pop_add(ch, 0, 0);
    }
    pick_show(copy_dst);
}

static void copy_start(void)
{
    pick_begin("COPY FROM PRESET", slot_pick_label, copy_preset_done, 0);
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (storage_state(n) != SLOT_EMPTY) {
            pop_add(n, 0, 0);
        }
    }
    pick_show(preset_number);
}

/* COPY ZONE: long-press DATA1 on a zone's sample (or ++): any zone of the
 * preset; the destination keeps its voltage (a new zone takes half the
 * range above). */
static int czone_dst;

static void czone_label(char *o, int v)
{
    const struct sample *m = zone_smp(v / ZONES, v % ZONES);

    o[0] = (char)('1' + v / ZONES);
    o[1] = ':';
    o[2] = (char)('1' + v % ZONES);
    o[3] = ' ';
    o[4] = 0;
    if (m) {
        copy_n(o + 4, m->name, 16);
    }
}

static void czone_done(int v)
{
    struct zone_cfg src = *zcfg(v / ZONES, v % ZONES);
    int n = zone_count(sel), old = czone_dst < n ? zcfg(sel, czone_dst)->smp - 1 : -1;

    if (czone_dst >= n) {
        zone_append(sel, &src, src.smp - 1);
        focus[P_ZONES] = 2 * czone_dst;
    } else {
        src.minv = zcfg(sel, czone_dst)->minv;
        irq_off();
        *zcfg(sel, czone_dst) = src;
        irq_on();
        zone_select(sel, czone_dst);
    }
    sr_follow(sel, czone_dst < n ? czone_dst : n, old, n);
    ui_say("ZONE COPIED");
}

static void hold_zsmp(int a)
{
    int z = a % ZONES;

    czone_dst = z;
    pick_begin("COPY ZONE FROM", czone_label, czone_done, 0);
    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int k = 0; k < zone_count(ch) && !preset.ch[ch].machine; k++) {
            pop_add(ch * ZONES + k, 0, 0);
        }
    }
    if (!pop.n) {
        ui_say("NO ZONES");
        return;
    }
    pick_show(sel * ZONES + (z < zone_count(sel) ? z : 0));
}

static void sample_op_menu(int smp)
{
    static const int items[] = { M_SOP_RENAME, M_SOP_ERASE };

    if (pop.open) {
        popup_close(0);
    }
    sop_down = 0;
    sop_idx = smp;
    copy_n(menu_title, samples[smp].name, 20);
    menu_open(menu_title, items, 2);
}

static void sample_rename_open(void)
{
    const char *nm = samples[sop_idx].name;
    size_t n = strlen(nm);
    char stem[FNAME_LEN + 1];

    if (n < 5 || n - 4 > FNAME_LEN) {
        ui_say("NAME TOO LONG");
        return;
    }
    copy_n(sop_ext, nm + n - 4, 4);
    copy_n(stem, nm, (int)n - 4);
    sop_ret = page;
    fname_open(FN_SAMPLE, stem);
    copy_n(fn_src, nm, 255);
}

static void hold_machine(int ch)
{
    static const int items[] = { M_COPY_CH };

    copy_dst = ch;
    menu_title[0] = 0;
    cat(menu_title, "CHANNEL ");
    put_uint(menu_title + 8, (unsigned)ch + 1, 1);
    menu_open(menu_title, items, 1);
}

/* ---- Channel Ops / Zone Ops (stock 2.0): double-click DATA1 on CHANNELS /
 * ZONES. Explode and Chop divide the region between a zone's sample start
 * and end into N slices (equal, or at the strongest onsets) and give each
 * its own channel or zone, loop points included; a stereo pair is done
 * together. ---- */

static int ops_page(void) { return page == P_CHANNELS || page == P_ZONES; }

static void slice_zone(struct zone_cfg *z, uint32_t fr, uint32_t a, uint32_t e)
{
    uint64_t l = (uint64_t)(e - a) * 32u;

    z->start = a;
    z->end = e >= fr ? POS_END : e;
    z->loop_start = a;
    z->loop_len = e >= fr || l >= POS_END ? POS_END : (uint32_t)l;
}

/* Slice boundaries of the region of zone z of channel ch; <0 with a toast
 * if they can't be found. */
static int zone_bounds(int ch, int z, int n, int chop, uint32_t *b)
{
    const struct sample *m = zone_smp(ch, z);
    const struct zone_cfg *c = zcfg(ch, z);
    uint32_t fr = zone_frames(ch, z), a = c->start, e = (uint32_t)z_end(c, fr);

    if (!m || !fr || e <= a || e - a < 8u * (uint32_t)n) {
        ui_say(!m || !fr ? "NO SAMPLE" : "SAMPLE TOO SHORT");
        return -1;
    }
    if (chop && m->state != SMP_READY) {
        storage_sample_demand(c->smp - 1);
        ui_say("SAMPLE NOT LOADED");
        return -1;
    }
    if (chop) {
        slices_chop(m->plane[0], m->plane[1], a, e, m->rate, n, b);
    } else {
        slices_equal(a, e, n, b);
    }
    return 0;
}

/* Voltage of the lowest CV selecting zone k of n (k = 0 is the top zone);
 * the keyboard modes put a boundary half a key below each note. */
static int bal_min(int mode, int k, int n)
{
    static const uint8_t white[7] = { 0, 2, 4, 5, 7, 9, 11 };
    int m = n - 1 - k, semi;

    if (k == n - 1) {
        return -500;
    }
    switch (mode) {
    case BAL_5V:
        return 500 - ((k + 1) * 1000 + n) / (2 * n);
    case BAL_10V:
        return 500 - ((k + 1) * 2000 + n) / (2 * n);
    default:
        semi = mode == BAL_KBD ? m : 12 * (m / 7) + white[m % 7];
        return ((2 * semi - 1) * 25 + 3) / 6;
    }
}

static void balance(int ch, int mode)
{
    int n = zone_count(ch);

    irq_off();
    for (int k = 0; k < n; k++) {
        zcfg(ch, k)->minv = (int16_t)bal_min(mode, k, n);
    }
    irq_on();
}

/* Calls f for the channel and its Stereo/Right side. */
static void both(int ch, void (*f)(int ch, int arg), int arg)
{
    int r = pair_of(ch);

    f(ch, arg);
    if (r >= 0) {
        f(r, arg);
    }
}

static void balance_arg(int ch, int mode) { balance(ch, mode); }

static void zone_flip(int ch, int n)
{
    irq_off();
    zones_flip(&preset.ch[ch].s, ops_z, n);
    irq_on();
}

static void zone_insert(int ch, int unused)
{
    (void)unused;
    irq_off();
    zones_insert(&preset.ch[ch].s, ops_z);
    irq_on();
}

static uint32_t b_zone[ZONES][SLICES_MAX + 1];

/* Explode / Chop into zones: the selected zone is the first slice. */
static void ops_zones(int chop, int n)
{
    int m = ops_ch, r = pair_of(m);
    uint32_t b[SLICES_MAX + 1];
    int side[2] = { m, r };

    if (zone_bounds(m, ops_z, n, chop, b) < 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        int ch = side[i];
        struct zone_cfg src;
        uint32_t fr;

        if (ch < 0 || ops_z >= zone_count(ch)) {
            continue;
        }
        src = *zcfg(ch, ops_z);
        fr = zone_frames(ch, ops_z);
        irq_off();
        for (int k = 0; k < n; k++) {
            struct zone_cfg z = src;

            slice_zone(&z, fr, b[k] < fr ? b[k] : fr - 1, b[k + 1] < fr ? b[k + 1] : fr);
            z.minv = ops_z + k < ZONES ? zcfg(ch, ops_z + k)->minv : -500;
            *zcfg(ch, ops_z + k) = z;
        }
        irq_on();
        balance(ch, ops_val[M_OP_BAL - M_OP_EXPLODE]);
    }
    ui_say(chop ? "CHOPPED" : "EXPLODED");
}

/* Explode / Chop into channels: each gets the channel whole, its zones
 * cut to the k-th slice. */
static void ops_channels(int chop, int n)
{
    int m = ops_ch, r = pair_of(m), step = r >= 0 ? 2 : 1, zn = zone_count(m);
    struct channel_cfg src[2];
    uint32_t fr[2][ZONES];

    for (int z = 0; z < zn; z++) {
        if (zone_bounds(m, z, n, chop, b_zone[z]) < 0) {
            return;
        }
    }
    src[0] = preset.ch[m];
    for (int z = 0; z < zn; z++) {
        fr[0][z] = zone_frames(m, z);
        fr[1][z] = r >= 0 ? zone_frames(r, z) : 0;
    }
    if (r >= 0) {
        src[1] = preset.ch[r];
    }
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < (r >= 0 ? 2 : 1); i++) {
            struct channel_cfg c = src[i];
            int dst = m + k * step + i;

            if (k) {
                c.s.xfade = 0;
                c.auto_trig = 0;
                c.mode = i ? MODE_STEREO : MODE_MASTER;
            }
            for (int z = 0; z < zn && z < ZONES; z++) {
                uint32_t f = fr[i][z];

                if (c.s.zone[z].smp && f > 8) {
                    uint32_t a = b_zone[z][k], e = b_zone[z][k + 1];

                    slice_zone(&c.s.zone[z], f, a < f ? a : f - 1, e < f ? e : f);
                }
            }
            put_channel(dst, &c, mix_now(), i ? r : m);
            if (k) {
                zsel[dst] = 0;
                engine_zone_lock[dst] = 0;
            }
        }
    }
    ui_say(chop ? "CHOPPED" : "EXPLODED");
}

static void ops_open(void)
{
    static const int chan[] = { M_CANCEL, M_OP_EXPLODE, M_OP_CHOP };
    static const int zones[] = { M_CANCEL, M_OP_BAL, M_OP_EXPLODE, M_OP_CHOP, M_OP_FLIP, M_OP_INSERT, M_OP_DELETE };
    int row = focus[P_CHANNELS] / XF_ROW_FIELDS;

    ops_zone = page == P_ZONES;
    /* On ZONES a Stereo/Right channel's ops act on it alone (stock). */
    ops_ch = ops_zone ? sel : preset_param_channel(row >= CHANNELS ? sel : row);
    if (!is_sampler(ops_ch)) {
        ui_say("SAMPLER CHANNEL ONLY");
        return;
    }
    if (!zone_count(ops_ch)) {
        ui_say("NO SAMPLE");
        return;
    }
    ops_z = ops_zone ? (zsel[sel] < zone_count(ops_ch) ? zsel[sel] : zone_count(ops_ch) - 1) : 0;
    ops_eng = 0;
    ops_ref = d2_now;
    ops_active = 1;
    menu_title[0] = 0;
    if (ops_zone) {
        cat(menu_title, "ZONE ");
        put_uint(menu_title + 5, (unsigned)ops_z + 1, 1);
        cat(menu_title, " OPS");
    } else {
        cat(menu_title, "CHANNEL ");
        put_uint(menu_title + 8, (unsigned)ops_ch + 1, 1);
        cat(menu_title, " OPS");
    }
    menu_open(menu_title, ops_zone ? zones : chan, ops_zone ? 7 : 3);
}

/* DATA2 sets the number of the highlighted item (the pot is absolute, so
 * it must be moved first). */
static void ops_data2(const struct panel *p)
{
    int v = pop_v(pop.idx), hi, lo = 2;
    static int last_idx = -1;

    if (pop.idx != last_idx) {
        last_idx = pop.idx;
        ops_eng = 0;
        ops_ref = p->data2;
    }
    if (v < M_OP_EXPLODE || v > M_OP_FLIP) {
        return;
    }
    hi = ops_max(v);
    if (v == M_OP_BAL) {
        lo = BAL_5V;
    }
    if (hi < lo) {
        return;
    }
    if (!ops_eng && (p->data2 > ops_ref + 48 || p->data2 + 48 < ops_ref)) {
        ops_eng = 1;
    }
    if (ops_eng) {
        ops_val[v - M_OP_EXPLODE] = (uint8_t)(lo + ((hi - lo) * p->data2 + 2047) / 4095);
    }
}

static void ops_run(int v)
{
    int n = ops_n(v);
    int mode = ops_val[M_OP_BAL - M_OP_EXPLODE];

    ops_active = 0;
    if (v != M_OP_BAL && v != M_OP_INSERT && v != M_OP_DELETE && !n) {
        ui_say(ops_zone ? "TOO FEW ZONES" : "NOT ENOUGH CHANNELS");
        return;
    }
    switch (v) {
    case M_OP_EXPLODE:
    case M_OP_CHOP:
        if (ops_zone) {
            ops_zones(v == M_OP_CHOP, n);
        } else {
            ops_channels(v == M_OP_CHOP, n);
        }
        break;
    case M_OP_BAL:
        both(ops_ch, balance_arg, mode);
        ui_say("ZONES BALANCED");
        break;
    case M_OP_FLIP:
        both(ops_ch, zone_flip, n);
        ui_say("ZONES FLIPPED");
        break;
    case M_OP_INSERT:
        both(ops_ch, zone_insert, 0);
        ui_say("ZONE INSERTED");
        break;
    case M_OP_DELETE:
        zone_delete(ops_ch, ops_z);
        focus[P_ZONES] = 2 * (ops_z < zone_count(sel) ? ops_z : ops_z ? ops_z - 1 : 0);
        ui_say("ZONE DELETED");
        break;
    }
}

enum { PK_DUP, PK_IMPORT, PK_TRUNC };
enum { IK_BROWSER, IK_CHANNEL, IK_PRESET };
static int prog_kind;
/* The import in progress: what to do with its files when the job ends. */
static struct {
    int kind, nt, tmp[SAMPLE_PREVIEWS], q[SAMPLE_PREVIEWS], dst, slot;
    int16_t zq[CHANNELS][ZONES];
} ic;

/* ---- Truncation: hold SAMPLE START and/or SAMPLE END 2 s on the sample
 * point pages. START cuts what is before the start point, END what is after
 * the end point; the zone (and its Stereo/Right twin) then plays the new
 * file <name>T00.wav. With PLAY MODE held the sample's own file is cut
 * instead, for every place that uses it, after a confirmation. ---- */

static void pos_button(int b);

static struct {
    int ch, z, r, smp, page, in_place;
    uint32_t from, to, frames;
} tr;
static int trunc_down, trunc_defer, trunc_shown;
static uint32_t trunc_at;

static void trunc_begin(void)
{
    if (!storage_trunc_start(tr.smp, tr.from, tr.to, tr.in_place)) {
        say_error("TRUNCATE FAILED");
        return;
    }
    prog_kind = PK_TRUNC;
    open_page(P_PROGRESS);
}

static void trunc_fire(int mask, int in_place)
{
    const struct zone_cfg *zc;
    uint32_t fr;

    tr.ch = sel;
    tr.z = cur_zone();
    zc = zcfg(tr.ch, tr.z);
    fr = zone_frames(tr.ch, tr.z);
    if (!storage_card()) {
        ui_say("NO CARD");
        return;
    }
    if (!zone_count(tr.ch) || !fr || zc->smp - 1 >= SAMPLES_MAX) {
        ui_say("NO SAMPLE");
        return;
    }
    tr.smp = zc->smp - 1;
    tr.frames = fr;
    tr.from = mask & (1 << PF_START) ? zc->start : 0;
    tr.to = mask & (1 << PF_END) ? (uint32_t)z_end(zc, fr) : fr;
    tr.in_place = in_place;
    tr.page = page;
    tr.r = pair_of(tr.ch);
    if (tr.r >= 0 && zcfg(tr.r, tr.z)->smp != zc->smp) {
        tr.r = -1;
    }
    if (tr.from == 0 && tr.to == fr) {
        ui_say("NOTHING TO TRUNCATE");
        return;
    }
    if (tr.to - tr.from < 8) {
        ui_say("REGION TOO SHORT");
        return;
    }
    if (in_place) {
        static const int items[] = { M_CANCEL, M_TR_GO };
        int others = storage_sample_uses(tr.smp) - 1 - (tr.r >= 0);

        menu_title[0] = 0;
        put_uint(menu_title, (unsigned)(others < 0 ? 0 : others), 1);
        cat(menu_title, " OTHER PLACES");
        menu_open(menu_title, items, 2);
        return;
    }
    trunc_begin();
}

static void trunc_buttons(const struct panel *p)
{
    static const uint8_t btn[2] = { BTN_SAMPLE_START, BTN_SAMPLE_END };
    uint32_t now = time_us();

    for (int b = 0; b < 2; b++) {
        uint32_t bit = 1u << btn[b];

        if (p->pressed & bit) {
            if (!trunc_down) {
                trunc_at = now;
                trunc_shown = 0;
            }
            if ((page == P_SPOS || page == P_LPOS) && is_sampler(sel)) {
                trunc_down |= 1 << b;
                if (page == P_SPOS) {
                    trunc_defer |= 1 << b;
                    continue;
                }
            }
            pos_button(b);
        }
        if (p->released & bit) {
            if ((trunc_defer >> b) & 1) {
                pos_button(b);
            }
            trunc_down &= ~(1 << b);
            trunc_defer &= ~(1 << b);
        }
    }
    if (p->pressed & ~((1u << BTN_SAMPLE_START) | (1u << BTN_SAMPLE_END))) {
        trunc_down = trunc_defer = 0;
    }
    if (!trunc_down) {
        return;
    }
    if (!trunc_shown && now - trunc_at >= 300000u) {
        trunc_shown = 1;
        ui_say("TRUNCATING IN 2 SEC");
    }
    if (now - trunc_at >= LONG_PRESS_US) {
        int mask = trunc_down, inp = play_held();

        trunc_down = trunc_defer = 0;
        play_held_used = 1;
        toast[0] = 0;
        trunc_fire(mask, inp);
    }
}

static void trunc_done(int st)
{
    int smp = storage_trunc_result();

    if (st != JOB_DONE || smp < 0) {
        say_error(st == JOB_CANCELLED ? "CANCELLED" : "TRUNCATE FAILED");
        open_page(tr.page);
        return;
    }
    if (tr.in_place) {
        storage_sample_trimmed(smp, tr.from, tr.to, tr.frames);
        storage_sample_demand(smp);
        ui_say("SAMPLE TRUNCATED");
    } else {
        irq_off();
        for (int i = 0; i < 2; i++) {
            int ch = i ? tr.r : tr.ch;

            if (ch >= 0) {
                struct zone_cfg *z = zcfg(ch, tr.z);

                z->smp = (uint16_t)(smp + 1);
                z->beats = 0;
                zone_trim(z, tr.from, tr.to, tr.frames);
            }
        }
        irq_on();
        storage_sample_demand(smp);
        ui_say("SAMPLE SAVED");
    }
    open_page(tr.page);
}

static const char *last_slash(const char *s)
{
    const char *r = 0;

    for (; *s; s++) {
        if (*s == '/') {
            r = s;
        }
    }
    return r;
}

static int ieq(const char *a, const char *b)
{
    while (*a && (*a | 32) == (*b | 32)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

/* Importing from another folder (LOAD, double-click on a folder): its
 * presets, then a preset's channels. Settings only; WAVs have to be copied
 * too once there are samples. */
static struct preset ximp;
static int x_ch, x_idx;
static char x_folder[24], x_title[24], x_dir[256];
static char ximp_names[CHANNELS][ZONES][SAMPLE_NAME_LEN + 1];

static void xpreset_label(char *o, int i)
{
    put_uint(o, (unsigned)storage_xslot(i), 3);
    cat(o, " ");
    cat(o, storage_xname(i));
}

static const char *ximp_sample(int ch, int z) { return ximp_names[ch][z]; }

static void xchan_label(char *o, int ch)
{
    chan_pick_label(o, ch, &ximp.ch[ch], ximp_sample);
}

static void new_or_slot_label(char *o, int v)
{
    if (v) {
        slot_pick_label(o, v);
        return;
    }
    memcpy(o, "NEW ", 5);
    put_uint(o + 4, (unsigned)storage_new_slot(), 3);
}

static void imp_menu(int v);
static void import_channel_go(void);
static void import_preset_go(int slot);

static void ximp_put(int v)
{
    int slot = v ? v : storage_new_slot();

    if (slot && storage_put(slot, &ximp) && storage_select(slot)) {
        ui_say("PRESET IMPORTED");
        open_page(P_HOME);
    } else {
        say_error("IMPORT FAILED");
    }
}

static void ximp_slot_done(int v)
{
    import_preset_go(v);
}

static void ximp_slot_pick(void)
{
    pick_begin("IMPORT TO", new_or_slot_label, ximp_slot_done, 0);
    if (storage_new_slot()) {
        pop_add(0, 0, 0);
    }
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (storage_state(n) != SLOT_EMPTY) {
            pop_add(n, 0, 0);
        }
    }
    pick_show(0);
}

static void xchan_done(int ch)
{
    static const int items[] = { M_IMP_CH };

    x_ch = ch;
    menu_title[0] = 0;
    cat(menu_title, "CHANNEL ");
    put_uint(menu_title + 8, (unsigned)ch + 1, 1);
    menu_open(menu_title, items, 1);
}

static void xchan_pick(int cur)
{
    pick_begin(x_title, xchan_label, xchan_done, 2);
    for (int ch = 0; ch < CHANNELS; ch++) {
        pop_add(ch, 0, 0);
    }
    pick_show(cur);
}

static int xload(int i)
{
    const struct preset *p = storage_xpreset(i);

    if (!p) {
        say_error("CAN'T READ");
        return 0;
    }
    ximp = *p;
    for (int c = 0; c < CHANNELS; c++) {
        for (int z = 0; z < ZONES; z++) {
            copy_n(ximp_names[c][z], storage_xsample(c, z), SAMPLE_NAME_LEN);
        }
    }
    x_idx = i;
    xpreset_label(x_title, i);
    x_title[20] = 0;
    return 1;
}

static void xpreset_done(int i)
{
    static const int items[] = { M_IMP_PRESET, M_IMP_CHS };

    if (xload(i)) {
        menu_open(x_title, items, 2);
    }
}

static void xpreset_pick(int cur)
{
    pick_begin(x_folder, xpreset_label, xpreset_done, 1);
    for (int i = 0; i < storage_xcount(); i++) {
        pop_add(i, 0, 0);
    }
    pick_show(cur);
}

static int import_target(int from, int *ch, int *z);
static void imp_open(int from, int ch, int z, const char *dir);

static void browse_open(int folder_index)
{
    copy_n(x_folder, storage_folder_name(folder_index), 20);
    copy_n(x_dir, storage_folder_name(folder_index), 255);
    if (storage_xopen(storage_folder_name(folder_index))) {
        xpreset_pick(0);
    } else {
        ui_say("NO PRESETS");
    }
}

/* An import made with the IMPORT browser that is still unsaved: LOAD then
 * reopens its directory (stock 2.0). Saving, reverting or loading another
 * folder ends it. */
static int imp_pending;
static char imp_pending_folder[64];
static char imp_last[200];
static int imp_last_set;
static void imp_click(int row);
static void imp_exit(void);

static int imp_pending_now(void)
{
    if (imp_pending && (imp_pending != preset_number || !storage_changed(preset_number) ||
                        !ieq(imp_pending_folder, storage_folder()))) {
        imp_pending = 0;
    }
    return imp_pending && imp_last_set;
}

/* LOAD page: a DATA1 click loads the folder once the double-click window
 * has passed; a double click opens the folder's presets for import. */
static int load_click_row = -1;
static uint32_t load_click_at;

static void load_click_poll(void)
{
    if (load_click_row < 0) {
        return;
    }
    if (page != P_LOAD || pop.open) {
        load_click_row = -1;
    } else if (time_us() - load_click_at >= DOUBLE_PRESS_US) {
        int row = load_click_row;

        load_click_row = -1;
        act_folder(row);
    }
}

static void load_click(int row)
{
    if (load_click_row == row && time_us() - load_click_at < DOUBLE_PRESS_US) {
        load_click_row = -1;
        browse_open(row);
        return;
    }
    load_click_row = row;
    load_click_at = time_us();
}

static void load_button(void)
{
    int ch, z;

    if ((page == P_HOME || page == P_CHANNELS || page == P_ZONES) && imp_pending_now() &&
        import_target(page, &ch, &z)) {
        imp_open(page, ch, z, imp_last);
        if (page == P_IMPORT) {
            ui_say("LOAD AGAIN: FOLDERS");
        }
        return;
    }
    open_page(P_LOAD);
}

static void save_menu(void)
{
    int items[6], n = 0;

    card_check();
    items[n++] = M_SAVE;
    if (storage_changed_count() > storage_changed(preset_number)) {
        items[n++] = M_SAVE_ALL;
    }
    items[n++] = M_SAVE_TO;
    items[n++] = M_REVERT;
    items[n++] = M_REVERT_CH;
    items[n++] = M_ERASE;
    menu_open(0, items, n);
}

static void folder_menu(void)
{
    static const int items[] = { M_CANCEL, M_LOAD, M_SAVE_LOAD };
    int dirty = storage_changed_count();

    menu_title[0] = 0;
    if (dirty) {
        put_uint(menu_title, (unsigned)dirty, 1);
        cat(menu_title, " UNSAVED");
    } else {
        cat(menu_title, "LOAD FOLDER?");
    }
    menu_open(menu_title, items, dirty ? 3 : 2);
}

static void install_menu(void)
{
    static const int items[] = { M_CANCEL, M_INSTALL };
    int dirty = storage_changed_count();

    menu_title[0] = 0;
    if (dirty) {
        put_uint(menu_title, (unsigned)dirty, 1);
        cat(menu_title, " UNSAVED");
    } else {
        cat(menu_title, "INSTALL APP?");
    }
    menu_open(menu_title, items, 2);
}

static void futil_menu(void)
{
    static const int items[] = { M_RENAME, M_DUP, M_FDELETE };

    copy_n(menu_title, fu_name, 20);
    menu_open(menu_title, items, 3);
}

/* 1 if v needs another click first. */
static int menu_confirm(int v)
{
    if (v != M_REVERT && v != M_REVERT_CH && v != M_ERASE && v != M_FDELETE && v != M_SOP_ERASE &&
        v != M_REC_ERASE) {
        return 0;
    }
    if (menu_armed == v || ((v == M_ERASE || v == M_SOP_ERASE) && !storage_card()) ||
        (v == M_FDELETE && storage_folder_in_use(fu_name))) {
        return 0;
    }
    menu_armed = v;
    return 1;
}

static void menu_do(int v)
{
    char msg[24] = "";
    int n;

    if ((v == M_SAVE || v == M_SAVE_ALL || v == M_SAVE_TO || v == M_ERASE || v == M_SAVE_LOAD) &&
        !storage_card()) {
        ui_say("NO CARD: NOT SAVED");
        return;
    }
    switch (v) {
    case M_SAVE:
        if (storage_save(preset_number)) {
            cat(msg, "SAVED ");
            put_uint(msg + 6, (unsigned)preset_number, 3);
            ui_say(msg);
        } else {
            say_error("SAVE FAILED");
        }
        break;
    case M_SAVE_ALL:
        n = storage_save_all();
        if (n < 0) {
            say_error("SAVE FAILED");
        } else {
            cat(msg, "SAVED ");
            put_uint(msg + 6, (unsigned)n, 1);
            cat(msg, n == 1 ? " PRESET" : " PRESETS");
            ui_say(msg);
        }
        break;
    case M_SAVE_TO:
        open_page(P_SAVE);
        break;
    case M_REVERT:
        storage_revert();
        ui_say("PRESET REVERTED");
        break;
    case M_REVERT_CH:
        storage_revert_channel(sel);
        ui_say("CHANNEL REVERTED");
        break;
    case M_ERASE:
        if (storage_erase()) {
            ui_say("PRESET ERASED");
        } else {
            say_error("ERASE FAILED");
        }
        break;
    case M_LOAD:
        load_folder_now();
        break;
    case M_SAVE_LOAD:
        if (storage_save_all() >= 0) {
            load_folder_now();
        } else {
            say_error("SAVE FAILED");
        }
        break;
    case M_RENAME:
        if (storage_folder_in_use(fu_name)) {
            ui_say("FOLDER IN USE");
        } else {
            fname_open(FN_RENAME, fu_name);
        }
        break;
    case M_DUP:
        fname_open(FN_DUP, fu_name);
        break;
    case M_COPY_CH:
        copy_start();
        break;
    case M_SOP_RENAME:
        if (!storage_card()) {
            ui_say("NO CARD");
        } else {
            sample_rename_open();
        }
        break;
    case M_SOP_ERASE:
        if (storage_sample_erase(sop_idx)) {
            ui_say("SAMPLE ERASED");
        } else {
            say_error("ERASE FAILED");
        }
        break;
    case M_IMP_PRESET:
        ximp_slot_pick();
        break;
    case M_IMP_CHS:
        xchan_pick(sel);
        break;
    case M_IMP_CH:
        import_channel_go();
        break;
    case M_IMP_NOW:
    case M_IMP_ADVANCE:
        imp_menu(v);
        break;
    case M_OP_EXPLODE: case M_OP_CHOP: case M_OP_BAL: case M_OP_FLIP: case M_OP_INSERT: case M_OP_DELETE:
        ops_run(v);
        break;
    case M_REC_OK:
    case M_REC_ERASE:
        rec_keep_pick(v == M_REC_ERASE);
        break;
    case M_TR_GO:
        trunc_begin();
        break;
    case M_CAL_ZERO:
    case M_CAL_GAIN:
        cal_capture(v == M_CAL_ZERO ? CAPK_ZERO : CAPK_GAIN, 1u << cal_menu_input);
        break;
    case M_CAL_SAVE:
    case M_CAL_SAVE_EXIT:
        cal_do_save(v == M_CAL_SAVE_EXIT);
        break;
    case M_HANG:
        /* WDT1 resets the module; the next boot says RECOVERED FROM HANG. */
        __asm__ volatile("cpsid i" ::: "memory");
        for (;;) {
        }
    case M_CAL_DISCARD:
        cal_discard();
        open_page(P_UTIL);
        ui_say("CAL DISCARDED");
        break;
    case M_INSTALL:
        if (!update_install()) {
            ui_say(update_error ? update_error : "INSTALL FAILED");
        }
        break;
    case M_FDELETE:
        if (storage_delete_folder(fu_name)) {
            prog_dup = 0;
            copy_n(prog_name, fu_name, 255);
            prog_idx = focus[P_FUTIL];
            open_page(P_PROGRESS);
        } else {
            say_error("DELETE FAILED");
        }
        break;
    }
}

/* ---- Importing samples: the whole card, preview, copy into the folder ----
 * Highlighting a file previews it on the target (a zone, or zone 1 of a
 * channel) like the sample popup does. CH buttons retarget (what was placed
 * stays); LOAD AND ADVANCE places and moves on. Any confirmation copies
 * every previewed file into the loaded folder and swaps the previews for
 * the folder's samples; leaving otherwise puts the preset back. ---- */

#define IMP_DIR_LEN 200
#define PREVIEW_DELAY_US 150000u

static struct {
    int active, committing, from, ch, z, slot, shown;
    char dir[IMP_DIR_LEN];
    struct preset snap;
    uint32_t moved_at;
    int tap_row;
    uint32_t tap_at;
    int down, used;
    uint32_t down_at;
} imp;
static int lload_down;
static uint32_t lload_at;

/* The channel and zone an import from `from` fills; 0 if there is none
 * (machine channel). */
static int import_target(int from, int *ch, int *z)
{
    int c = sel, zz = 0;

    if (from == P_CHANNELS && drawn_page == P_CHANNELS && nfields) {
        const struct field_ref *r = &fields[focus[P_CHANNELS] < nfields ? focus[P_CHANNELS] : 0];
        if (r->f == &f_chsmp) {
            c = r->arg / ZONES;
        } else if (r->f == &f_machine || r->f == &f_mode || r->f == &f_xgroup || r->f == &f_mix_on) {
            c = r->arg;
        }
    } else if (from == P_ZONES && drawn_page == P_ZONES && nfields) {
        const struct field_ref *r = &fields[focus[P_ZONES] < nfields ? focus[P_ZONES] : 0];
        zz = r->f == &f_zsmp ? r->arg % ZONES : cur_zone();
    }
    if (!is_sampler(c)) {
        return 0;
    }
    *ch = c;
    *z = zz;
    return 1;
}

static int imp_up(void) { return imp.dir[0] != 0; }

static int imp_rows(void) { return imp_up() + storage_bcount(); }

/* The entry of a list row: -1 for "..". */
static int imp_entry(int row) { return row - imp_up(); }

static void imp_list(const char *select)
{
    if (!storage_bopen(imp.dir)) {
        imp.dir[0] = 0;
        storage_bopen("");
    }
    focus[P_IMPORT] = 0;
    d2_engaged = 0;
    d2_ref = d2_now;
    for (int i = 0; select && i < storage_bcount(); i++) {
        if (!strcmp_(storage_bname(i), select)) {
            focus[P_IMPORT] = i + imp_up();
        }
    }
    imp.shown = -2;
    imp.moved_at = time_us();
    imp.tap_row = -1;
    copy_n(imp_last, imp.dir, IMP_DIR_LEN - 1);
    imp_last_set = 1;
}

static void imp_preview_row(int row)
{
    int e = imp_entry(row), idx = -1;

    if (e >= 0 && !storage_bisdir(e)) {
        const char *nm = storage_bname(e);

        if (ieq(imp.dir, storage_folder())) {
            idx = samples_find(nm);
        } else {
            idx = storage_bpreview(e);
            if (idx >= 0 && samples[idx].state == SMP_BAD) {
                ui_say(samples[idx].err);
                storage_preview_release(idx);
                idx = -1;
            } else if (idx < 0) {
                ui_say("CAN'T PREVIEW");
            }
        }
    }
    pv_step(idx);
}

static void imp_leave_state(void)
{
    engine_virtual_buttons = 0;
    aud_mask = 0;
    pv_end();
    storage_preview_release_all();
    imp.active = 0;
}

static void imp_cancel(void)
{
    if (!imp.active) {
        return;
    }
    pv_restore();
    if (preset_number == imp.slot) {
        irq_off();
        preset = imp.snap;
        irq_on();
    } else {
        storage_put(imp.slot, &imp.snap);
    }
    imp_leave_state();
}

static void imp_cancel_if_browsing(void)
{
    if (imp.active && !imp.committing) {
        imp_cancel();
    }
}

static void imp_exit(void)
{
    int from = imp.from;

    imp_cancel();
    open_page(from);
}

static void imp_open(int from, int ch, int z, const char *dir)
{
    if (!storage_card() || !storage_folder()[0]) {
        ui_say("NO FOLDER");
        return;
    }
    imp.from = from;
    imp.ch = ch;
    imp.z = z;
    imp.slot = preset_number;
    imp.snap = preset;
    imp.committing = 0;
    imp.down = 0;
    copy_n(imp.dir, dir, IMP_DIR_LEN - 1);
    open_page(P_IMPORT);
    imp.active = 1;
    pv_begin(ch, z);
    imp_list(0);
}

static void imp_retarget(int ch, int z)
{
    engine_zone_lock[pv.ch] = pv.lock;
    engine_virtual_buttons = 0;
    aud_mask = 0;
    imp.ch = ch;
    imp.z = z;
    pv_begin(ch, z);
    imp_preview_row(focus[P_IMPORT]);
}

/* Moves the target on after a placement: the next zone when the import
 * started on ZONES, else the next channel (past a Stereo/Right pair). */
static void imp_advance(void)
{
    int ch = imp.ch, z = imp.z;

    if (imp.from == P_ZONES) {
        z++;
    } else {
        ch += ch + 1 < CHANNELS && preset.ch[ch + 1].mode == MODE_STEREO ? 2 : 1;
        z = 0;
    }
    if (z >= ZONES || ch >= CHANNELS || !is_sampler(ch)) {
        ui_say("NO MORE TARGETS");
        return;
    }
    sel = ch;
    imp_retarget(ch, z);
}

static void imp_finish(void)
{
    int from = imp.from;

    imp_pending = preset_number;
    copy_n(imp_pending_folder, storage_folder(), 63);
    imp.committing = 0;
    imp_leave_state();
    open_page(from);
}

/* Copies the previewed files in; the swap happens when the job ends. */
static void imp_commit(void)
{
    int n = 0;

    storage_import_clear();
    for (int i = SAMPLES_MAX; i < SAMPLES_MAX + SAMPLE_PREVIEWS; i++) {
        char dir[IMP_DIR_LEN];
        const char *p, *sl;
        size_t dn;

        if (!samples[i].path || !storage_sample_used(i)) {
            continue;
        }
        p = samples[i].path + 2;
        sl = last_slash(p);
        dn = (size_t)(sl - p);
        copy_n(dir, p, (int)dn);
        ic.tmp[n] = i;
        ic.q[n] = storage_import_add(dir, sl + 1);
        if (ic.q[n] < 0) {
            ui_say("TOO MANY FILES");
            return;
        }
        n++;
    }
    ic.nt = n;
    if (!n) {
        imp_finish();
        return;
    }
    if (!storage_import_start()) {
        say_error("IMPORT FAILED");
        return;
    }
    ic.kind = IK_BROWSER;
    imp.committing = 1;
    prog_kind = PK_IMPORT;
    open_page(P_PROGRESS);
}

static void imp_swap_done(void)
{
    static struct preset q;

    q = preset;
    for (int k = 0; k < ic.nt; k++) {
        int r = storage_import_result(ic.q[k]);

        if (r < 0) {
            preset_drop_sample(&q, (unsigned)ic.tmp[k] + 1);
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            for (int z = 0; z < ZONES; z++) {
                if (q.ch[c].s.zone[z].smp == ic.tmp[k] + 1) {
                    q.ch[c].s.zone[z].smp = (uint16_t)(r + 1);
                }
            }
        }
        storage_sample_demand(r);
    }
    irq_off();
    preset = q;
    irq_on();
}

static void imp_click(int row)
{
    int e = imp_entry(row);

    if (e < 0) {
        char *sl = (char *)last_slash(imp.dir);
        char from[SAMPLE_NAME_LEN + 1];

        copy_n(from, sl ? sl + 1 : imp.dir, SAMPLE_NAME_LEN);
        if (sl) {
            *sl = 0;
        } else {
            imp.dir[0] = 0;
        }
        pv_step(-1);
        imp_list(from);
    } else if (storage_bisdir(e)) {
        size_t n = strlen(imp.dir);

        if (n + strlen(storage_bname(e)) + 2 >= IMP_DIR_LEN) {
            ui_say("PATH TOO LONG");
            return;
        }
        if (n) {
            imp.dir[n++] = '/';
        }
        copy_n(imp.dir + n, storage_bname(e), IMP_DIR_LEN - 1 - (int)n);
        pv_step(-1);
        imp_list(0);
    } else if (imp.tap_row == row && time_us() - imp.tap_at < DOUBLE_PRESS_US) {
        imp.tap_row = -1;
        if (imp.shown != row) {
            imp.shown = row;
            imp_preview_row(row);
        }
        imp_advance();
    } else {
        imp.tap_row = row;
        imp.tap_at = time_us();
    }
}

static void imp_poll(void)
{
    uint32_t now = time_us();

    if (!imp.active || page != P_IMPORT || pop.open) {
        return;
    }
    if (imp.tap_row >= 0 && now - imp.tap_at >= DOUBLE_PRESS_US) {
        static const int items[] = { M_IMP_NOW, M_IMP_ADVANCE };

        imp.tap_row = -1;
        menu_open(0, items, 2);
    }
    if (imp.shown != focus[P_IMPORT] && now - imp.moved_at >= PREVIEW_DELAY_US) {
        imp.shown = focus[P_IMPORT];
        imp_preview_row(imp.shown);
    }
}

static void imp_input(const struct panel *p)
{
    uint32_t now = time_us();
    int rows = imp_rows();

    if (p->data1_steps) {
        int n = focus[P_IMPORT] + p->data1_steps;

        focus[P_IMPORT] = n < 0 ? 0 : n >= rows ? rows - 1 : n;
        imp.moved_at = now;
        imp.tap_row = -1;
        d2_engaged = 0;
        d2_ref = p->data2;
    } else if (rows > 1) {
        int n = d2_jump(p->data2, rows);

        if (n >= 0 && n != focus[P_IMPORT]) {
            focus[P_IMPORT] = n;
            imp.moved_at = now;
            imp.tap_row = -1;
        }
    }
    if (p->pressed & (1u << BTN_DATA1)) {
        imp.down = 1;
        imp.used = 0;
        imp.down_at = now;
    }
    if (imp.down && (p->held & (1u << BTN_DATA1))) {
        if (!imp.used && now - imp.down_at >= LONG_PRESS_US) {
            imp.used = 1;
            if (pv.cur < 0) {
                imp.shown = focus[P_IMPORT];
                imp_preview_row(imp.shown);
            }
            imp_commit();
        }
    } else if (imp.down) {
        imp.down = 0;
        if (!imp.used && rows) {
            imp_click(focus[P_IMPORT]);
        }
    }
}

static void imp_menu(int v)
{
    if (v == M_IMP_ADVANCE) {
        imp_advance();
    } else {
        imp_commit();
    }
}


/* Channel and preset import from another folder: the zones' WAVs come along
 * (same rules), then the settings are put in. */
static int queue_zones(const struct channel_cfg *c, int ch, int16_t *zq)
{
    int n = 0;

    for (int z = 0; z < ZONES; z++) {
        zq[z] = -1;
        if (!c->machine && ximp_names[ch][z][0]) {
            zq[z] = (int16_t)storage_import_add(x_dir, ximp_names[ch][z]);
            n += zq[z] >= 0;
        }
    }
    return n;
}

/* The zones whose files arrived, closed up. */
static void zones_imported(struct channel_cfg *c, const int16_t *zq)
{
    struct zone_cfg keep[ZONES];
    int n = 0;

    if (c->machine) {
        return;
    }
    for (int z = 0; z < ZONES; z++) {
        int r = zq[z] >= 0 ? storage_import_result(zq[z]) : -1;

        if (r >= 0) {
            keep[n] = c->s.zone[z];
            keep[n++].smp = (uint16_t)(r + 1);
        }
    }
    for (int z = 0; z < ZONES; z++) {
        if (z < n) {
            c->s.zone[z] = keep[z];
        } else {
            zone_defaults(&c->s.zone[z]);
        }
    }
}

static void import_start_job(int kind)
{
    if (!storage_import_start()) {
        say_error("IMPORT FAILED");
        return;
    }
    ic.kind = kind;
    prog_kind = PK_IMPORT;
    open_page(P_PROGRESS);
}

static void import_channel_go(void)
{
    storage_import_clear();
    ic.dst = sel;
    if (queue_zones(&ximp.ch[x_ch], x_ch, ic.zq[0])) {
        import_start_job(IK_CHANNEL);
        return;
    }
    put_channel(sel, &ximp.ch[x_ch], mix_of(ximp.mixfx), x_ch);
    ui_say("CHANNEL IMPORTED");
    open_page(P_HOME);
}

static void import_preset_go(int slot)
{
    int n = 0;

    storage_import_clear();
    ic.slot = slot;
    for (int c = 0; c < CHANNELS; c++) {
        n += queue_zones(&ximp.ch[c], c, ic.zq[c]);
    }
    if (n) {
        import_start_job(IK_PRESET);
        return;
    }
    ximp_put(slot);
}

/* The import job ended: finish what asked for it. */
static void import_job_done(int st)
{
    if (st != JOB_DONE) {
        say_error(st == JOB_CANCELLED ? "CANCELLED" : "IMPORT FAILED");
        if (ic.kind == IK_BROWSER) {
            int from = imp.from;

            imp.committing = 0;
            imp_cancel();
            open_page(from);
        } else {
            open_page(P_HOME);
        }
        return;
    }
    switch (ic.kind) {
    case IK_BROWSER:
        imp_swap_done();
        imp_finish();
        ui_say("IMPORTED");
        break;
    case IK_CHANNEL: {
        struct channel_cfg c = ximp.ch[x_ch];

        zones_imported(&c, ic.zq[0]);
        put_channel(ic.dst, &c, mix_of(ximp.mixfx), x_ch);
        ui_say("CHANNEL IMPORTED");
        open_page(P_HOME);
        break;
    }
    case IK_PRESET:
        for (int c = 0; c < CHANNELS; c++) {
            zones_imported(&ximp.ch[c], ic.zq[c]);
        }
        ximp_put(ic.slot);
        break;
    }
}

/* While a duplicate or delete runs, any button cancels the duplicate (the
 * delete can't be stopped); the page ends when the job does. */
static int progress_input(const struct panel *p)
{
    int st = storage_job_state();

    if (page != P_PROGRESS) {
        return 0;
    }
    if (storage_job_active()) {
        if (p->pressed && (st == JOB_COPYING || st == JOB_IMPORTING || st == JOB_TRUNCATING)) {
            channels_held_used = 1;
            play_held_used = 1;
            storage_job_cancel();
        }
        return 1;
    }
    storage_job_clear();
    if (prog_kind == PK_IMPORT) {
        prog_kind = PK_DUP;
        import_job_done(st);
        return 1;
    }
    if (prog_kind == PK_TRUNC) {
        prog_kind = PK_DUP;
        trunc_done(st);
        return 1;
    }
    if (st == JOB_FAILED) {
        say_error("FAILED");
    } else {
        ui_say(st == JOB_CANCELLED ? "CANCELLED" : prog_dup ? "DUPLICATED" : "DELETED");
    }
    open_page(P_FUTIL);
    futil_focus(prog_name, prog_idx);
    return 1;
}

/* LOAD SOFTWARE: while the copy runs any button cancels it; once installed
 * the page shows that for a moment, then the module resets. DATA1 offers
 * the install (FOUND) or looks again. */
static uint32_t sw_done_at;

static int software_input(const struct panel *p)
{
    int st = update_state();

    if (page != P_SOFTWARE) {
        return 0;
    }
    if (update_busy()) {
        if (p->pressed && st != UPD_SCANNING) {
            channels_held_used = 1;
            play_held_used = 1;
            update_cancel();
            ui_say("CANCELLED");
            update_find();
        }
        return 1;
    }
    if (st == UPD_DONE) {
        if (sw_done_at && time_us() - sw_done_at > 1000000u) {
            update_reset();
        }
        return 1;
    }
    if (pop.open || !(p->pressed & (1u << BTN_DATA1))) {
        return 0;
    }
    if (st == UPD_FOUND) {
        install_menu();
    } else {
        card_check();
        update_find();
    }
    return 1;
}

/* A channel button: PLAY MODE held latches/unlatches it (stock), a
 * latched channel's button releases it, else it triggers. */
static void channel_press(const struct panel *p, int ch)
{
    if ((p->held & (1u << BTN_PLAY_MODE)) || ((engine_status.latched >> ch) & 1)) {
        engine_latch(ch);
        play_held_used = 1;
    } else {
        engine_trigger(ch);
    }
}


/* DATA1 held on a field whose click waits for the release (long press on a
 * machine, press-and-turn on a pitch). */
static struct {
    int on, used, pitch, zoom, page;
    uint32_t at;
} press;

static uint64_t dc_at;          /* a DATA1 click on CHANNELS/ZONES, for the double click */
static int dc_fine;

static void field_click(const struct field_ref *r)
{
    if (ops_page()) {
        dc_at = time_us64();
        dc_fine = fine;
    }
    if (r->f->action) {
        r->f->action(r->arg);
    } else if (r->f->list) {
        popup_open(r);
    } else if (r->f->set) {
        fine = !fine;
        d2_engaged = 0;
        d2_ref = d2_now;
    }
}

/* Press-and-turn on the pitch: whole semitones (stock). */
static void pitch_snap(const struct field_ref *r, int steps)
{
    int v = r->f->get(r->arg);

    for (; steps > 0; steps--) {
        v = (v >= 0 ? v / 100 : -((-v + 99) / 100)) * 100 + 100;
    }
    for (; steps < 0; steps++) {
        v = (v > 0 ? (v + 99) / 100 : -(-v / 100)) * 100 - 100;
    }
    edit(r, v);
}

static void popup_up(void)
{
    if (pop.ref.f->list == L_MACHINE) {
        mpop_fill(1, pop.mfam);
    } else {
        struct field_ref m = { &f_machine, pop.ref.arg / ZONES };

        popup_close(0);
        popup_open(&m);
    }
}

/* Returns 1 if a popup is open (it takes all input). */
static int popup_input(const struct panel *p)
{
    uint32_t others = p->pressed & ~(1u << BTN_DATA1);

    if (!pop.open) {
        ops_active = 0;
        return 0;
    }
    if (p->data1_steps) {
        dc_at = 0;
    }
    if ((p->pressed & (1u << BTN_DATA1)) && dc_at && time_us64() - dc_at < DOUBLE_PRESS_US && ops_page() &&
        pop.ref.f->list != L_MENU && pop.ref.f->list != L_PICK) {
        dc_at = 0;
        popup_close(0);
        fine = dc_fine;
        ops_open();
        return 1;
    }
    if (ops_active) {
        ops_data2(p);
    }
    if (pop.pv_due && ((others & (0xffu << BTN_CH1)) ||
                       time_us() - pop.moved_at >= POP_PREVIEW_US)) {
        pop.pv_due = 0;
        popup_preview();
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (others & (1u << (BTN_CH1 + ch))) {
            channel_press(p, ch);
            others &= ~(1u << (BTN_CH1 + ch));
        }
    }
    if (others == (1u << BTN_LOAD) && !pop.browse && pop_v(0) == -3 &&
        (pop.ref.f->list == L_SAMPLE ||
         (pop.ref.f->list == L_MACHINE && pop.mlevel == 2))) {
        popup_up();
        return 1;
    }
    if (others == (1u << BTN_LOAD) && pop.browse) {
        if (pop.browse == 2) {
            xpreset_pick(x_idx);
        } else {
            pop.open = 0;
        }
        return 1;
    }
    if (others) {
        /* Their releases must not act either. */
        channels_held_used = 1;
        play_held_used = 1;
        popup_close(0);
        return 1;
    }
    if (pop.ref.f->list == L_SAMPLE) {
        int v = pop_v(pop.idx);

        if (p->pressed & (1u << BTN_DATA1)) {
            sop_at = time_us();
            sop_down = 1;
            sop_used = 0;
        } else if (sop_down && (p->held & (1u << BTN_DATA1))) {
            if (!sop_used && SMP_INDEX(v) >= 0 && time_us() - sop_at >= LONG_PRESS_US) {
                sop_used = 1;
                sample_op_menu(SMP_INDEX(v));
                return 1;
            }
        } else if (sop_down) {
            sop_down = 0;
            if (!sop_used) {
                if (v == -3) {
                    popup_up();
                } else {
                    popup_close(1);
                }
                return 1;
            }
        }
    } else if ((p->pressed & (1u << BTN_DATA1)) && pop.ref.f->list == L_MACHINE) {
        int v = pop_v(pop.idx);

        if (pop.mlevel == 1 && v == POP_OFF) {
            int ch = pop.ref.arg;

            popup_close(0);
            chan_set_off(ch);
        } else if (pop.mlevel == 1 && v > 0) {
            mpop_fill(2, v - 1);
        } else if (pop.mlevel == 1) {
            struct field_ref s = { &f_chsmp, pop.ref.arg * ZONES };

            struct channel_cfg origin = pop.saved;

            popup_close(0);
            edit(&(struct field_ref){ &f_machine, s.arg / ZONES }, 0);
            popup_open(&s);
            if (pop.open) {
                pop_origin = origin;
                pop_origin_set = 1;
            }
        } else if (v == -3) {
            popup_up();
        } else {
            popup_close(1);
        }
        return 1;
    } else if (p->pressed & (1u << BTN_DATA1)) {
        int v = pop_v(pop.idx);

        if (pop.ref.f->list == L_PICK) {
            popup_close(0);
            pick_done(v);
        } else if (pop.ref.f->list != L_MENU) {
            popup_close(1);
        } else if (!menu_confirm(v)) {
            popup_close(0);
            menu_do(v);
        }
        return 1;
    }
    if (p->data1_steps) {
        int was = pop_v(pop.idx);

        pop_move(pop.idx + p->data1_steps);
        menu_armed = -1;
        d2_engaged = 0;
        d2_ref = p->data2;
        if (pop_v(pop.idx) != was || pop.pv_due) {
            pop.pv_due = 0;
            popup_preview();
        }
    } else if (pop.ref.f->list != L_MENU && pop.n > 1) {
        /* The preview waits until it rests. */
        int k = d2_jump(p->data2, pop.n);

        if (k >= 0) {
            int was = pop_v(pop.idx);

            pop_move(k);
            if (pop_v(pop.idx) != was) {
                pop.pv_due = 1;
            }
        }
    }
    return 1;
}

/* ---- A8 home: DATA1 scrolls the folder's presets, click loads; with the
 * click held, every slot (empty ones too) loads as you scroll. ---- */

#define BROWSE_US 5000000u

static int browse_num, browse_active, browse_turned;
static uint32_t browse_until;

static void browse_load(int slot)
{
    char msg[24] = "LOADED ";

    if (storage_select(slot)) {
        put_uint(msg + 7, (unsigned)slot, 3);
        ui_say(msg);
    } else {
        say_error("CAN'T LOAD");
    }
}

/* Cancel Folder Load (2.0): DATA1 held 2 s on Home while a folder loads;
 * releasing or turning first lets the load go on. */
static int cancel_down;
static uint32_t cancel_at;

static void home_browse(const struct panel *p)
{
    int held = (p->held & (1u << BTN_DATA1)) != 0;
    uint32_t now = time_us();

    if (p->pressed & (1u << BTN_DATA1)) {
        browse_turned = 0;
        cancel_down = storage_loading();
        cancel_at = now;
    }
    if (cancel_down && (!held || p->data1_steps || !storage_loading())) {
        cancel_down = 0;
    } else if (cancel_down && now - cancel_at >= LONG_PRESS_US) {
        cancel_down = 0;
        browse_turned = 1;
        browse_active = 0;
        if (storage_cancel_load()) {
            ui_say("LOAD CANCELLED");
        } else {
            say_error("CAN'T CANCEL");
        }
        return;
    }
    if (p->data1_steps) {
        int n = browse_active ? browse_num : preset_number;
        int dir = p->data1_steps > 0 ? 1 : -1;
        for (int k = 0; k < (p->data1_steps * dir); k++) {
            n = storage_next(n, dir, held);
        }
        browse_num = n;
        browse_active = 1;
        browse_until = now + BROWSE_US;
        if (held) {
            browse_turned = 1;
            browse_load(n);
            browse_active = 0;
        }
    }
    if ((p->released & (1u << BTN_DATA1)) && !browse_turned && browse_active) {
        if (storage_state(browse_num) != SLOT_EMPTY) {
            browse_load(browse_num);
        }
        browse_active = 0;
    }
    if (browse_active && (int32_t)(now - browse_until) > 0) {
        browse_active = 0;
    }
}

/* ---- Sampling (stock's Sampling Setup page, SAMPLE button, arming) ---- */

static void draw_header(const char *title);
static void draw_channel_column(void);

static int rs_lock(void)
{
    if (rec_state() >= REC_RECORDING) {
        ui_say("NOT WHILE SAMPLING");
        return 1;
    }
    return 0;
}

#define RCHOICE(name, member, lock, ...) \
    static const char *const name##_labels[] = { __VA_ARGS__ }; \
    static int get_##name(int a) { (void)a; return rec_setup.member; } \
    static void set_##name(int a, int v) \
    { (void)a; if (!(lock && rs_lock())) { rec_setup.member = (uint8_t)v; rec_setup_changed(); } } \
    static void range_##name(int a, int *lo, int *hi) \
    { (void)a; *lo = 0; *hi = (int)(sizeof(name##_labels) / sizeof(name##_labels[0])) - 1; } \
    static void fmt_##name(char *o, int v, int a) { (void)a; o[0] = 0; cat(o, name##_labels[v]); } \
    static const struct field f_##name = { get_##name, set_##name, range_##name, fmt_##name, 0, L_VLIST, \
                                           name##_labels, 0 };

RCHOICE(rdest, dest, 0, "UNASSIGNED", "CHNL.1", "CHNL.2", "CHNL.3", "CHNL.4", "CHNL.5", "CHNL.6", "CHNL.7", "CHNL.8",
        "EMPTY CH")
RCHOICE(rzone, zone_empty, 0, "ZONE 1", "EMPTY ZN")
RCHOICE(rmode, mode, 1, "ONCE", "CIRCULAR", "SOS", "SOS>>")
RCHOICE(rrate, rate, 1, "48KHZ", "96KHZ", "192KHZ")
RCHOICE(rname, naming, 0, "NEW001", "S001", "PNM001")
RCHOICE(rhpf, hpf, 0, "DC CPL", "AC CPL")
RCHOICE(rhot, hot, 0, "SAFE", "*HOT*")
RCHOICE(rside, side, 1, "LEFT", "RIGHT", "STEREO", "DUAL M.")
RCHOICE(rmonl, mon_l, 0, "L OFF", "L ON")
RCHOICE(rmonr, mon_r, 0, "R OFF", "R ON")

static int get_rtime(int a) { (void)a; return rec_setup.time; }
static void set_rtime(int a, int v) { (void)a; if (!rs_lock()) { rec_setup.time = (int16_t)v; rec_setup_changed(); } }
static void range_rtime(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = RTIME_MAX; }
static void fmt_rtime(char *o, int v, int a)
{
    (void)a;
    if (!v) {
        memcpy(o, "ALL", 4);
        return;
    }
    fmt_fixed(o, v, 1, 3, 0);
    cat(o, "S");
}
static const struct field f_rtime = { get_rtime, set_rtime, range_rtime, fmt_rtime, 0, L_NONE, 0, 0 };

static int get_rthr(int a) { (void)a; return rec_setup.thresh; }
static void set_rthr(int a, int v) { (void)a; rec_setup.thresh = (int16_t)v; rec_setup_changed(); }
static void range_rthr(int a, int *lo, int *hi) { (void)a; *lo = RTHRESH_MIN; *hi = CV_INPUTS; }
static void fmt_rthr(char *o, int v, int a)
{
    (void)a;
    o[0] = 0;
    if (v < 0) {
        fmt_int(o, v);
        cat(o, "DB");
    } else if (!v) {
        cat(o, "OFF");
    } else {
        char s[4] = { (char)('1' + (v - 1) / 3), (char)('A' + (v - 1) % 3), 0, 0 };
        cat(o, "CV ");
        cat(o, s);
    }
}
static const struct field f_rthr = { get_rthr, set_rthr, range_rthr, fmt_rthr, 0, L_NONE, 0, 0 };

static int get_rlevel(int a) { (void)a; return rec_setup.level; }
static void set_rlevel(int a, int v) { (void)a; rec_setup.level = (int8_t)v; rec_setup_changed(); }
static void range_rlevel(int a, int *lo, int *hi) { (void)a; *lo = RLEVEL_MIN; *hi = RLEVEL_MAX; }
static void fmt_rlevel(char *o, int v, int a) { (void)a; fmt_fixed(o, v, 0, 2, 1); }
static const struct field f_rlevel = { get_rlevel, set_rlevel, range_rlevel, fmt_rlevel, 0, L_NONE, 0, 0 };

static int get_rmlvl(int a) { (void)a; return rec_setup.mon_level; }
static void set_rmlvl(int a, int v) { (void)a; rec_setup.mon_level = (int8_t)v; rec_setup_changed(); }
static void range_rmlvl(int a, int *lo, int *hi) { (void)a; *lo = RMON_OFF; *hi = RMON_MAX; }
static void fmt_rmlvl(char *o, int v, int a)
{
    (void)a;
    if (v <= RMON_OFF) {
        memcpy(o, "OFF", 4);
        return;
    }
    fmt_fixed(o, v, 0, 2, 1);
}
static const struct field f_rmlvl = { get_rmlvl, set_rmlvl, range_rmlvl, fmt_rmlvl, 0, L_NONE, 0, 0 };

/* a: 0 reverb, 1 delay */
static int get_rmsend(int a) { return a ? rec_setup.mon_dly : rec_setup.mon_rev; }
static void set_rmsend(int a, int v)
{
    if (a) {
        rec_setup.mon_dly = (uint8_t)v;
    } else {
        rec_setup.mon_rev = (uint8_t)v;
    }
    rec_setup_changed();
}
static void range_rmsend(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = RMON_SEND_MAX; }
static void fmt_rmsend(char *o, int v, int a) { (void)a; o[0] = 0; fmt_int(o, v); cat(o, "%"); }
static const struct field f_rmsend = { get_rmsend, set_rmsend, range_rmsend, fmt_rmsend, 0, L_NONE, 0, 0 };

/* The take just recorded: where it was put (for ERASE). Once it is saved
 * an OK / ERASE popup opens (stock 3.04 does so on any page), and again on
 * Sampling Setup, for 30 s; after that it is kept. */
#define KEEP_US 30000000u
static struct {
    int l, r, slot, active, offer, saved;
    uint8_t mask;
    uint32_t at;
    char name[SAMPLE_NAME_LEN + 1];
    struct channel_cfg ch[CHANNELS];
} keep;

static int empty_channel(int from)
{
    for (int c = from; c < CHANNELS; c++) {
        if (is_sampler(c) && !zone_count(c) && preset.ch[c].mode != MODE_STEREO) {
            return c;
        }
    }
    return -1;
}

static void place_one(int smp, int ch)
{
    int z = 0, n;

    if (ch < 0 || ch >= CHANNELS) {
        return;
    }
    keep.mask |= (uint8_t)(3u << ch);
    if (!is_sampler(ch)) {
        set_machine(ch, 0);
    }
    n = zone_count(ch);
    if (rec_setup.zone_empty) {
        z = n < ZONES ? n : ZONES - 1;
    }
    if (z < n) {
        struct zone_cfg *zc = zcfg(ch, z);
        irq_off();
        zc->start = 0;
        zc->end = POS_END;
        zc->loop_start = 0;
        zc->loop_len = POS_END;
        irq_on();
    }
    assign_sample(ch, z, smp, 0);
    z = z < zone_count(ch) ? z : zone_count(ch) - 1;
    zone_mirror(ch, z, ZM_POS);
    zone_select(ch, z);
}

static void rec_place(int l, int r)
{
    int d = rec_setup.dest, ch = d >= 1 && d <= CHANNELS ? d - 1 : d == RDEST_EMPTY ? empty_channel(0) : -1;
    char msg[24] = "";

    memset(&keep, 0, sizeof(keep));
    keep.l = l;
    keep.r = r;
    keep.slot = preset_number;
    copy_n(keep.name, samples[l].name, SAMPLE_NAME_LEN);
    memcpy(keep.ch, preset.ch, sizeof(keep.ch));
    if (d == RDEST_EMPTY && ch < 0) {
        memcpy(msg, "NO EMPTY CHANNEL", 17);
    }
    if (ch >= 0) {
        place_one(l, ch);
        if (r >= 0) {
            place_one(r, d == RDEST_EMPTY ? empty_channel(ch + 1) : ch + 1);
        }
        memcpy(msg, "SAMPLE TO CH ", 14);
        msg[13] = (char)('1' + ch);
        msg[14] = 0;
    }
    keep.active = 1;
    if (msg[0]) {
        ui_say(msg);
    }
}

/* Erase: the channels go back to how they were and the files are deleted. */
static void rec_keep_pick(int erase)
{
    if (!keep.active) {
        return;
    }
    keep.active = 0;
    if (!erase) {
        return;
    }
    if (samples_find(keep.name) != keep.l) {
        ui_say("SAMPLE GONE");
        return;
    }
    if (keep.slot == preset_number) {
        irq_off();
        for (int c = 0; c < CHANNELS; c++) {
            if ((keep.mask >> c) & 1u) {
                preset.ch[c] = keep.ch[c];
            }
        }
        irq_on();
    }
    if (keep.r >= 0) {
        storage_sample_unload(keep.r);
        storage_sample_erase(keep.r);
    }
    storage_sample_unload(keep.l);
    ui_say(storage_sample_erase(keep.l) ? "SAMPLE ERASED" : "ERASE FAILED");
}

static void rec_ui_poll(void)
{
    static int last_state;
    const char *m = rec_message();
    int l, r, st = rec_state();

    if (m) {
        ui_say(m);
    }
    if (rec_take(&l, &r)) {
        rec_place(l, r);
    }
    if (last_state == REC_SAVING && st != REC_SAVING && keep.active) {
        keep.saved = 1;
        keep.at = time_us();
        keep.offer = 1;
    }
    last_state = st;
    if (keep.active && keep.saved && time_us() - keep.at >= KEEP_US) {
        keep.active = 0;
        if (pop.open && pop.title && !strcmp_(pop.title, "SAMPLE SAVED.")) {
            popup_close(0);
        }
    }
    if (keep.active && keep.offer && !pop.open && page != P_PROGRESS) {
        static const int items[] = { M_REC_OK, M_REC_ERASE };
        keep.offer = 0;
        menu_open("SAMPLE SAVED.", items, 2);
    }
    rec_metering(page == P_RSETUP);
}

static int setup_down, setup_used;
static uint32_t setup_at;

static void setup_buttons(const struct panel *p)
{
    if (p->pressed & (1u << BTN_SAMPLING_SETUP)) {
        setup_down = 1;
        setup_used = 0;
        setup_at = time_us();
    }
    if (setup_down && !setup_used && (p->held & (1u << BTN_SAMPLING_SETUP)) &&
        time_us() - setup_at >= LONG_PRESS_US) {
        setup_used = 1;
        if (rec_state() >= REC_RECORDING) {
            ui_say("SAMPLING");
        } else if (rec_armed()) {
            rec_arm(0);
            ui_say("SAMPLING DISARMED");
        } else {
            rec_arm(1);
        }
    }
    if ((p->released & (1u << BTN_SAMPLING_SETUP)) && setup_down) {
        setup_down = 0;
        if (!setup_used) {
            page_button(P_RSETUP);
            if (page == P_RSETUP && keep.active && keep.saved) {
                keep.offer = 1;
            }
        }
    }
}

/* SETUP + CHn: the sampling destination (stock 2.0). */
static int setup_channel(const struct panel *p, int ch)
{
    char msg[24] = "SAMPLING DEST: CH ";

    if (!(p->held & (1u << BTN_SAMPLING_SETUP))) {
        return 0;
    }
    setup_used = 1;
    rec_setup.dest = (uint8_t)(ch + 1);
    rec_setup_changed();
    msg[18] = (char)('1' + ch);
    msg[19] = 0;
    ui_say(msg);
    return 1;
}

/* Meters, 0 to -24 dB (stock marks 0/-6/-12/-18), y from 15 down to 98. */
#define RM_TOP  14
#define RM_BOT  88
static int meter_y(float lin)
{
    float db = 0.0f;
    int y;

    if (lin <= 0.0631f) {
        return RM_BOT;
    }
    while (lin < 1.0f && db > -24.0f) {
        lin *= 1.12201845f;
        db -= 1.0f;
    }
    y = RM_TOP + (int)((-db) * (float)(RM_BOT - RM_TOP) / 24.0f);
    return y < RM_TOP ? RM_TOP : y > RM_BOT ? RM_BOT : y;
}

static void callout(int x, int y, const char *s)
{
    int w = gfx_text_width(s) + 4;

    gfx_rect(x, y - 1, w, 9, C_DIM);
    gfx_text(x + 2, y, s, C_BLACK);
}

static void draw_rec_window(void)
{
    char s[24];
    uint32_t n = rec_frames(), cap = rec_capacity(), rate = rec_rate_hz();
    int st = rec_state(), x;

    gfx_rect(10, 13, 118, 90, C_BLACK);
    gfx_frame(10, 13, 118, 90, C_DIM);
    for (int c = 0; c < 2; c++) {
        int y = 18 + c * 16;
        gfx_frame(14, y, 110, 12, C_DIM);
        x = cap ? (int)((uint64_t)n * 108u / cap) : 0;
        gfx_rect(15, y + 1, x, 10, C_FAINT);
        gfx_rect(15 + x, y + 1, 1, 10, C_RED);
    }
    put_uint(s, n, 8);
    cat(s, " SMPLS");
    gfx_text(18, 56, s, C_WHITE);
    {
        uint32_t t = rate ? (uint32_t)((uint64_t)n * 10u / rate) : 0;
        fmt_fixed(s, (int)t, 1, 4, 0);
        cat(s, " SECONDS");
        gfx_text(18, 68, s, C_WHITE);
    }
    if (st == REC_FINISHING) {
        gfx_text(18, 84, "PLEASE WAIT", C_YELLOW);
    } else if (st == REC_SAVING) {
        memcpy(s, "SAVING ", 8);
        put_uint(s + 7, (unsigned)rec_save_percent(), 1);
        cat(s, "%");
        gfx_text(18, 84, s, C_YELLOW);
    }
}

static void draw_rsetup(void)
{
    static const struct field *const col[] = { &f_rdest, &f_rzone, &f_rmode, &f_rtime, &f_rrate, &f_rname,
                                               &f_rhpf, &f_rhot };
    static const char *const names[] = { "DEST", "ZONE DEST", "REC MODE", "TIME", "RATE", "NAMING",
                                         "HIGH PASS", "ARMING" };
    int busy = rec_state() >= REC_RECORDING;

    draw_header(busy ? "SAMPLING" : "SAMPLING SETUP");
    for (int c = 0; c < 2; c++) {
        int x = c ? 34 : 14, y = meter_y(rec_peak[c]);
        gfx_frame(x, RM_TOP - 1, 6, RM_BOT - RM_TOP + 2, C_YELLOW);
        if (y < RM_BOT) {
            gfx_rect(x + 1, y, 4, RM_BOT - y, rec_peak[c] >= 0.999f ? C_RED : C_YELLOW);
        }
    }
    for (int k = 0; k < 4; k++) {
        static const char *const lab[] = { "0", "6", "12", "18" };
        int y = RM_TOP + k * (RM_BOT - RM_TOP) / 4 - 3;
        gfx_text(k ? 21 : 24, y, lab[k], C_YELLOW);
    }
    if (rec_setup.thresh < 0) {
        int y = meter_y(rec_thresh_lin());
        gfx_rect(9, y - 2, 1, 5, C_YELLOW);
        gfx_rect(10, y - 1, 1, 3, C_YELLOW);
        gfx_rect(11, y, 2, 1, C_YELLOW);
    }
    gfx_frame(43, RM_TOP - 1, 5, RM_BOT - RM_TOP + 2, C_TITLE);
    {
        int h = (rec_setup.level - RLEVEL_MIN) * (RM_BOT - RM_TOP) / (RLEVEL_MAX - RLEVEL_MIN);
        gfx_rect(44, RM_BOT - h, 3, h, C_TITLE);
    }
    for (int i = 0; i < 8; i++) {
        int y = 13 + i * 10;
        if (field(0, y, col[i], 0, 127)) {
            callout(12, y, names[i]);
        }
    }
    if (field(12, 94, &f_rthr, 0, 0)) {
        callout(12, 83, "THRESHOLD");
    }
    if (field(52, 94, &f_rside, 0, 0)) {
        callout(52, 83, "SIDE");
    }
    if (field(0, 94, &f_rlevel, 0, 127)) {
        callout(95, 83, "LEVEL");
    }
    gfx_text(12, 106, "MON", C_DIM);
    field(36, 106, &f_rmonl, 0, 0);
    field(72, 106, &f_rmonr, 0, 0);
    if (field(0, 106, &f_rmlvl, 0, 127)) {
        callout(62, 95, "MON LEVEL");
    }
    field_dim = !(rec_setup.mon_l || rec_setup.mon_r) || rec_setup.mon_level <= RMON_OFF;
    gfx_text(12, 117, "REV", C_DIM);
    if (field(0, 117, &f_rmsend, 0, 59)) {
        callout(12, 106, "MON TO REVERB");
    }
    gfx_text(72, 117, "DLY", C_DIM);
    if (field(0, 117, &f_rmsend, 1, 127)) {
        callout(62, 106, "MON TO DELAY");
    }
    field_dim = 0;
    if (busy) {
        draw_rec_window();
    }
}

/* Top line on every page: armed, or the take so far. */
static void draw_rec_notice(void)
{
    char s[28];
    int st = rec_state();

    if (rec_armed()) {
        gfx_rect(0, 0, 128, 11, C_BLACK);
        gfx_text(16, 1, "*SAMPLING ARMED*", C_RED);
    } else if (st == REC_RECORDING) {
        uint32_t rate = rec_rate_hz(), t = (uint32_t)((uint64_t)rec_frames() * 10u / rate);
        memcpy(s, "*SAMPLING* ", 12);
        fmt_fixed(s + 11, (int)t, 1, 1, 0);
        cat(s, " S");
        gfx_rect(0, 0, 128, 11, C_BLACK);
        gfx_text(1, 1, s, C_RED);
    } else if (st == REC_SAVING || st == REC_FINISHING) {
        memcpy(s, "SAVING SAMPLE ", 15);
        put_uint(s + 14, (unsigned)rec_save_percent(), 1);
        cat(s, "%");
        gfx_rect(0, 0, 128, 11, C_BLACK);
        gfx_text(1, 1, s, C_YELLOW);
    }
}

/* ---- Sampler page buttons ---- */

static uint32_t zones_used, zones_at, lend_at;
static int lend_down;
static char notice[28];
static uint64_t notice_until;

/* Pages where a Stereo/Right channel shows its own values, offsets from
 * its master's (rview); home keeps the view for the next page. */
static int rview_page(void)
{
    switch (page) {
    case P_HOME: case P_PITCH: case P_LEVEL: case P_MUTATE: case P_ENVELOPE: case P_PHASE:
    case P_MACHINE: case P_MACHINE_CV: case P_EQ: case P_EQCV:
        return 1;
    }
    return 0;
}

/* Stereo/Right channels play their master's parameters, so parameter
 * pages select the master: the pages above as the right side's view (its
 * offsets), the others with a notice (stock 2.0); Channels, Zones,
 * Pan/Mix (its own pan) and Filter keep the Stereo/Right side. */
static void sr_redirect(void)
{
    int keep = page == P_CHANNELS || page == P_ZONES || page == P_PANMIX || page == P_FILTER || page >= P_UTIL;

    if (rview >= 0 && (preset.ch[rview].mode != MODE_STEREO || preset_param_channel(rview) != sel)) {
        rview = -1;
    }
    if (page >= P_UTIL) {
        return;
    }
    if (rview >= 0 && !rview_page()) {
        sel = rview;
        rview = -1;
        focus_moved();
    }

    int m = preset_param_channel(sel);

    if (m == sel || keep) {
        return;
    }
    if (rview_page()) {
        rview = sel;
        sel = m;
        focus_moved();
        return;
    }
    sel = m;
    focus_moved();
    memcpy(notice, "STEREO: CH ", 12);
    notice[11] = (char)('1' + m);
    memcpy(notice + 12, " SELECTED", 10);
    notice_until = time_us64() + 2000000u;
}

static const char *const pos_title[4] = { "SAMPLE START", "SAMPLE END", "LOOP START", "LOOP END" };

/* The page for the selected channel's type: sampler point/zone pages vs
 * the machine pages behind the same buttons. */
static void page_fix(void)
{
    int smp = is_sampler(sel), to = -1, f = 0;

    if (!smp && (page == P_SPOS || page == P_LPOS)) {
        sampler_only_title = pos_title[(page == P_SPOS ? 0 : 2) + (focus[page] == 1)];
        to = P_SAMPLER_ONLY;
    } else if (!smp && page == P_ZONES) {
        to = P_MACHINE;
    } else if (smp && page == P_SAMPLER_ONLY) {
        for (int b = 0; b < 4; b++) {
            if (sampler_only_title == pos_title[b]) {
                to = b < 2 ? P_SPOS : P_LPOS;
                focus[to] = b & 1;
            }
        }
    } else if (smp && (page == P_MACHINE || page == P_MACHINE_CV)) {
        to = P_ZONES;
    }
    if (to >= 0) {
        f = focus[to];
        open_page(to);
        focus[to] = f;
    }
}

/* SAMPLE START/END, LOOP START/END: the paired buttons pick the field, the
 * same one again closes; an open edit field stays open across the four
 * (2.0). Machines: the sampler-only page. */
static void pos_button(int b)
{
    int target = b < PF_LSTART ? P_SPOS : P_LPOS, keep = fine && (page == P_SPOS || page == P_LPOS);

    if (!is_sampler(sel)) {
        sampler_only(pos_title[b]);
        return;
    }
    if (page == target && focus[target] == (b & 1)) {
        open_page(P_HOME);
        return;
    }
    if (page != target) {
        open_page(target);
    }
    focus[target] = b & 1;
    focus_moved();
    fine = keep;
}

/* ZONES acts on release (ZONES + CHn selects a zone); held on a sampler
 * channel it toggles Zone Lock (2.0: a double click) and leaves the page
 * as it is. */
static void zones_click(void)
{
    if (!is_sampler(sel)) {
        open_page(page == P_MACHINE ? P_MACHINE_CV : page == P_MACHINE_CV ? P_HOME : P_MACHINE);
        return;
    }
    page_button(P_ZONES);
}

static void zones_hold(void)
{
    int on = !engine_zone_lock[sel];

    memset((void *)engine_zone_lock, 0, sizeof(engine_zone_lock));
    if (on && zone_count(sel)) {
        engine_zone_lock[sel] = (uint8_t)(cur_zone() + 1);
    }
    ui_say(engine_zone_lock[sel] ? "ZONE LOCKED" : "ZONE UNLOCKED");
}

static int gr_size_field(const struct field_ref *r);
static int gr_size_d2(int d2);

/* CALIBRATE: a capture takes every button (DATA1 measures or goes back,
 * anything else cancels); UTILITY leaves, asking first when edited; SAVE
 * offers the save; other page buttons stay on the page. */
static int cal_input(const struct panel *p)
{
    static const int leave_items[] = { M_CAL_SAVE_EXIT, M_CAL_DISCARD };
    static const int save_items[] = { M_CANCEL, M_CAL_SAVE };
    uint32_t others = p->pressed & ~((1u << BTN_DATA1) | (0xffu << BTN_CH1));

    if (page != P_CAL || pop.open) {
        return 0;
    }
    if (cal_capture_state() != CAP_NONE) {
        if (p->pressed & (1u << BTN_DATA1)) {
            cal_capture_next();
        } else if (p->pressed) {
            cal_capture_cancel();
            ui_say("CANCELLED");
        }
        channels_held_used = play_held_used = zones_used = 1;
        return 1;
    }
    if (!others) {
        return 0;
    }
    channels_held_used = play_held_used = zones_used = 1;
    if (others & (1u << BTN_UTILITY)) {
        if (cal_dirty()) {
            copy_n(menu_title, "UNSAVED CAL", 23);
            menu_open(menu_title, leave_items, 2);
        } else {
            open_page(P_UTIL);
        }
    } else if (others & (1u << BTN_SAVE)) {
        copy_n(menu_title, "SAVE CAL.BIN?", 23);
        menu_open(menu_title, save_items, 2);
    }
    return 1;
}

/* Pages that use the front card look for it (re)inserted or pulled twice a
 * second: CMD13 when mounted, else a card init (a few ms without a card). */
static void card_poll(void)
{
    static uint32_t at;
    int was = storage_card();

    if ((page != P_LOAD && page != P_FUTIL && page != P_SAVE && page != P_UTIL && page != P_SOFTWARE &&
         page != P_SDCARD) ||
        pop.open || storage_job_active() || update_busy() || time_us() - at < 500000u) {
        return;
    }
    at = time_us();
    card_check();
    if (storage_card() == was) {
        return;
    }
    if (!was) {
        storage_scan();
    } else {
        ui_say("NO CARD");
    }
    if (page == P_LOAD || page == P_FUTIL || page == P_SDCARD) {
        open_page(page);
    }
}

static void ui_input_main(const struct panel *p)
{
    uint32_t pr = p->pressed;

    last_panel = p;
    if (p->data1_steps) {
        dc_at = 0;
    }
    d2_now = p->data2;
    bench_tick();
    aud_poll();
    load_click_poll();
    card_poll();
    if (progress_input(p) || software_input(p) || cal_input(p) || popup_input(p)) {
        return;
    }
    imp_poll();

    if (pr & (1u << BTN_PLAY_MODE)) {
        play_held_used = play_d2_used = 0;
        play_d2_ref = p->data2;
    }
    if (pr & (1u << BTN_CHANNELS)) {
        channels_held_used = 0;
    }
    if (pr & (1u << BTN_ZONES)) {
        zones_used = 0;
        zones_at = time_us();
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (pr & (1u << (BTN_CH1 + ch))) {
            if (setup_channel(p, ch)) {
                continue;
            }
            if (p->held & (1u << BTN_ZONES)) {
                /* ZONES + CHn selects zone n (stock 1.x), from any page. */
                zones_used = 1;
                if (is_sampler(sel) && ch < zone_count(sel)) {
                    zone_select(sel, ch);
                    focus[P_ZONES] = 2 * ch;
                }
                continue;
            }
            if (ch != sel || rview >= 0) {
                /* A new channel: DATA2 must be moved again before it edits
                 * (else the pot's position lands on the new channel). */
                focus_moved();
            }
            sel = ch;
            rview = -1;
            if (page == P_CHANNELS && focus[P_CHANNELS] >= XF_FIELD_GROUP) {
                if (preset.ch[ch].s.xfade) {
                    xf_show = preset.ch[ch].s.xfade - 1;
                } else {
                    char m[] = "CH n: NO GROUP";
                    m[3] = (char)('1' + ch);
                    ui_say(m);
                }
            } else if (page == P_CHANNELS) {
                focus[P_CHANNELS] = ch * XF_ROW_FIELDS;
                focus_moved();
            }
            if (page == P_IMPORT && imp.active && ch != imp.ch) {
                if (is_sampler(ch)) {
                    imp_retarget(ch, 0);
                } else {
                    ui_say("SAMPLER CHANNEL ONLY");
                }
            }
            if (p->held & (1u << BTN_CHANNELS)) {
                channels_held_used = 1;
            } else {
                channel_press(p, ch);
            }
        }
    }
    if ((p->released & (1u << BTN_CHANNELS)) && !channels_held_used) {
        open_page(page == P_CHANNELS ? P_REVERB : page == P_REVERB ? P_DELAY : page == P_DELAY ? P_MIXCOMP : page == P_MIXCOMP ? P_HOME : P_CHANNELS);
    }
    if (pr & (1u << BTN_LOAD)) {
        if (page == P_IMPORT) {
            lload_down = 1;
        } else if (page == P_LOAD) {
            lload_down = 2;
        } else {
            load_button();
        }
        lload_at = time_us();
    }
    if (lload_down && (p->held & (1u << BTN_LOAD)) && time_us() - lload_at >= LONG_PRESS_US) {
        lload_down = 0;
        pref_auto_audition = !pref_auto_audition;
        storage_prefs_changed();
        ui_say(pref_auto_audition ? "AUTO-AUDITION ON" : "AUTO-AUDITION OFF");
    }
    if ((p->released & (1u << BTN_LOAD)) && lload_down) {
        int d = lload_down;

        lload_down = 0;
        if (d == 1 && page == P_IMPORT) {
            if (last_slash(imp.dir)) {
                imp_click(0);
            } else {
                char top[IMP_DIR_LEN];

                copy_n(top, imp.dir, IMP_DIR_LEN - 1);
                imp_cancel();
                open_page(P_LOAD);
                for (int i = 0; top[0] && i < storage_folders(); i++) {
                    if (ieq(storage_folder_name(i), top)) {
                        focus[P_LOAD] = i;
                    }
                }
            }
        } else if (d == 2 && page == P_LOAD) {
            open_page(P_HOME);
        }
    }
    if (pr & (1u << BTN_SAVE)) {
        if (page == P_SAVE) {
            open_page(P_HOME);
        } else {
            save_menu();
            return;
        }
    }
    if (pr & (1u << BTN_UTILITY)) {
        open_page(page == P_UTIL ? P_HOME : P_UTIL);
    }
    if ((p->held & (1u << BTN_ZONES)) && !zones_used && is_sampler(sel) &&
        time_us() - zones_at >= LONG_PRESS_US) {
        zones_used = 1;
        zones_hold();
    }
    if ((p->released & (1u << BTN_ZONES)) && !zones_used) {
        zones_click();
    }
    trunc_buttons(p);
    if (pr & (1u << BTN_LOOP_START)) pos_button(PF_LSTART);
    /* L.END on the loop page waits for the release: held 2 s it toggles
     * END/LEN (stock). */
    if (pr & (1u << BTN_LOOP_END)) {
        if (page == P_LPOS) {
            lend_at = time_us();
            lend_down = 1;
        } else {
            pos_button(PF_LEND);
        }
    }
    if (lend_down && (p->held & (1u << BTN_LOOP_END)) && time_us() - lend_at >= LONG_PRESS_US) {
        lend_down = 0;
        if (page == P_LPOS) {
            uint8_t *e = &preset.ch[sel].s.loop_is_end;
            *e = !*e;
            ui_say(*e ? "LOOP END" : "LOOP LENGTH");
        }
    }
    if ((p->released & (1u << BTN_LOOP_END)) && lend_down) {
        lend_down = 0;
        pos_button(PF_LEND);
    }
    if (pr & (1u << BTN_PITCH)) open_page(page == P_PITCH ? P_TLOCK : page == P_TLOCK ? P_HOME : P_PITCH);
    if (pr & (1u << BTN_LEVEL)) page_button(P_LEVEL);
    if (pr & (1u << BTN_PAN_MIX)) {
        open_page(page == P_PANMIX ? P_COMP : page == P_COMP ? P_SENDS : page == P_SENDS ? P_HOME : P_PANMIX);
    }
    /* A page cycle ends on home. */
    if (pr & (1u << BTN_MUTATE)) open_page(page == P_MUTATE ? P_EQ : page == P_EQ ? P_EQCV : page == P_EQCV ? P_HOME : P_MUTATE);
    if (pr & (1u << BTN_PHASE_MOD)) open_page(page == P_PHASE ? P_GRAIN : page == P_GRAIN ? P_GRAINCV : page == P_GRAINCV ? P_HOME : P_PHASE);
    if (pr & (1u << BTN_ENVELOPE)) open_page(page == P_ENVELOPE ? P_FILTER : page == P_FILTER ? P_HOME : P_ENVELOPE);
    if (pr & (1u << BTN_LOOP_MODE)) {
        int pc = preset_param_channel(sel);
        if (is_sampler(pc)) {
            static const char *const names[] = { "LOOP OFF", "LOOP NORMAL", "LOOP GATED" };
            uint8_t *m = &preset.ch[pc].s.loop_mode;
            *m = (uint8_t)((*m + 1) % 3);
            ui_say(names[*m]);
        } else {
            sampler_only("LOOP MODE");
        }
    }
    /* A8: hold PLAY MODE and turn DATA2 on home to pick the D2CV input. */
    if ((p->held & (1u << BTN_PLAY_MODE)) && page == P_HOME &&
        (play_d2_used || p->data2 > play_d2_ref + 48 || p->data2 + 48 < play_d2_ref)) {
        int v = (p->data2 * (CV_INPUTS + 1)) / 4096;
        char msg[16] = "D2CV ";
        if (!play_d2_used || v != preset.d2cv) {
            preset.d2cv = (uint8_t)v;
            fmt_d2cv(msg + 5, v, 0);
            ui_say(msg);
        }
        play_held_used = play_d2_used = 1;
    }
    if ((p->released & (1u << BTN_PLAY_MODE)) && !play_held_used) {
        uint8_t *m = &preset.ch[preset_param_channel(sel)].s.play_mode;
        *m = !*m;
        ui_say(*m ? "1 SHOT" : "GATED");
    }
    if ((pr & (1u << BTN_SAMPLE)) && rec_button()) {
        pr &= ~(1u << BTN_SAMPLE);
    }
    if (pr & (1u << BTN_SAMPLE)) {
        int ch, z, from = page >= P_UTIL ? P_HOME : page;

        if (page == P_IMPORT) {
            imp_exit();
        } else if (import_target(from, &ch, &z)) {
            imp_open(from, ch, z, imp_last_set ? imp_last : "");
        } else {
            ui_say("SAMPLER CHANNEL ONLY");
        }
    }
    setup_buttons(p);

    if (page == P_HOME && preset.d2cv) {
        cv_d2_volts = (float)p->data2 * (10.0f / 4095.0f) - 5.0f;
    }
    cv_d2_input = page == P_CAL ? 0 : preset.d2cv;

    sr_redirect();
    page_fix();
    if (page == P_IMPORT) {
        imp_input(p);
        return;
    }
    if (page == P_HOME) {
        home_browse(p);
        return;
    }
    /* Field input only against the fields this page last drew. */
    if (drawn_page != page || !nfields) {
        return;
    }
    int *f = &focus[page];
    if (*f >= nfields) {
        *f = nfields - 1;
    }
    const struct field_ref *r = &fields[*f];
    uint32_t now = time_us();

    if (press.on && press.page != page) {
        press.on = 0;
    }
    if ((pr & (1u << BTN_DATA1)) && ops_page() && dc_at && time_us64() - dc_at < DOUBLE_PRESS_US) {
        dc_at = 0;
        fine = dc_fine;
        press.on = 0;
        ops_open();
        return;
    }
    if (pr & (1u << BTN_DATA1)) {
        press.pitch = page == P_PITCH && fine &&
                      ((r->f == &f_dest && r->arg == D_PITCH) ||
                       (r->f == &f_sval && (r->arg == SV_PITCH || r->arg == SV_ZPITCH)) || rofs_pitch(r));
        press.zoom = page == P_SPOS || page == P_LPOS;
        if (press.pitch || press.zoom || r->f->hold || (fine && fx_coarse(r->f))) {
            press.on = 1;
            press.used = 0;
            press.page = page;
            press.at = now;
        } else {
            field_click(r);
            return;
        }
    }
    if (press.on) {
        /* A click that can also be a long press, or the start of a
         * press-and-turn, waits for the release. */
        if (p->data1_steps) {
            if (press.pitch) {
                pitch_snap(r, p->data1_steps);
                press.used = 1;
                return;
            }
            if (fine && fx_coarse(r->f)) {
                edit(r, fx_coarse(r->f)(r->arg, r->f->get(r->arg), p->data1_steps));
                d2_engaged = 0;
                d2_ref = p->data2;
                press.used = 1;
                return;
            }
            if (press.zoom) {
                /* Press-and-turn: waveform zoom 1-32x (2.0). */
                int z = pos_zoom;
                for (int k = p->data1_steps; k > 0 && z < 32; k--) {
                    z *= 2;
                }
                for (int k = p->data1_steps; k < 0 && z > 1; k++) {
                    z /= 2;
                }
                pos_zoom = z;
                press.used = 1;
                return;
            }
            press.used = 1;
        }
        if (!press.used && r->f->hold && now - press.at >= LONG_PRESS_US) {
            press.on = 0;
            r->f->hold(r->arg);
            return;
        }
        if (!(p->held & (1u << BTN_DATA1))) {
            press.on = 0;
            if (!press.used) {
                field_click(r);
                return;
            }
        }
    }
    if (p->data1_steps) {
        if (fine && r->f->set) {
            const struct param_choice *ch = r->f == &f_dest ? dest_choice(r->arg) : rofs_choice(r);

            if (ch) {
                int i = param_choice_index(ch, r->f->get(r->arg)) + p->data1_steps;

                edit(r, param_choice_value(ch, i < 0 ? 0 : i >= ch->n ? ch->n - 1 : i));
            } else {
                edit(r, r->f->get(r->arg) + p->data1_steps);
            }
            /* DATA2 lets go until it moves again, or it would undo this. */
            d2_engaged = 0;
            d2_ref = p->data2;
        } else {
            int n = *f + p->data1_steps;
            *f = n < 0 ? 0 : n >= nfields ? nfields - 1 : n;
            focus_moved();
        }
    }
    if (page == P_LOAD || page == P_FUTIL) {
        int k = d2_jump(p->data2, nfields);

        if (k >= 0) {
            *f = k;
        }
        return;
    }
    /* DATA2 doesn't sweep zone samples ("- -" would delete). */
    if (page == P_HOME || !r->f->set || r->f->list == L_SAMPLE || (r->f->list == L_RVPROG && !rv_has_programs())) {
        return;
    }
    if (!d2_engaged && (p->data2 > d2_ref + 48 || p->data2 + 48 < d2_ref)) {
        d2_engaged = 1;
    }
    if (d2_engaged) {
        int lo, hi;
        r->f->range(r->arg, &lo, &hi);
        edit(r, gr_size_field(r) ? gr_size_d2(p->data2) : lo + (int)(((int64_t)(hi - lo) * p->data2 + 2047) / 4095));
    }
}

static int utility_function_page(void)
{
    switch (page) {
    case P_CVMON: case P_PANEL: case P_AUDIO: case P_BENCH: case P_PREFS: case P_INCDEC:
    case P_ABOUT: case P_FUTIL: case P_FNAME: case P_PROGRESS: case P_MEMORY: case P_SDCARD:
    case P_SOFTWARE: case P_CAL:
        return 1;
    }
    return 0;
}

/* Screen saver: after the idle time the display goes off; the first button
 * press or encoder turn wakes it and is not passed on (nor the release of
 * a button that woke it). Gates and DATA2 don't wake it. */
void ui_input(const struct panel *p)
{
    static struct panel q;
    static uint32_t swallow, active_at;
    static int asleep;
    uint32_t now = time_us();
    int woke = p->pressed || p->data1_steps;

    rec_ui_poll();
    if (asleep) {
        if (woke) {
            oled_sleep(0);
            asleep = 0;
            active_at = now;
            swallow = p->held;
        }
        return;
    }
    if (woke || p->released || utility_function_page() || !active_at) {
        active_at = now;
    } else if (pref_saver && now - active_at >= (uint32_t)pref_saver * 60000000u) {
        asleep = 1;
        oled_sleep(1);
        return;
    }
    if (swallow) {
        q = *p;
        q.pressed &= ~swallow;
        q.released &= ~swallow;
        q.held &= ~swallow;
        swallow &= p->held;
        p = &q;
    }
    ui_input_main(p);
}

/* ---- Drawing ---- */

static void draw_lr_meters(void)
{
    gfx_text(0, 0, "L", C_GREEN);
    gfx_text(0, 8, "R", C_RED);
    gfx_rect(7, 2, (int)(engine_status.mix_peak[0] * 14.0f), 3, C_GREEN);
    gfx_rect(7, 10, (int)(engine_status.mix_peak[1] * 14.0f), 3, C_RED);
}

/* The live preset's first zone sample of a sampler channel, -1 if none. */
static int channel_sample(int ch)
{
    for (int z = 0; z < YML_ZONES; z++) {
        int i = storage_sample(preset_number, ch, z);
        if (i >= 0) {
            return i;
        }
    }
    return -1;
}

/* A machine, or a sampler channel whose zones' samples are all in RAM. */
static int channel_ready(int ch)
{
    int any = 0;

    if (preset.ch[ch].machine) {
        return 1;
    }
    for (int z = 0; z < YML_ZONES; z++) {
        int i = storage_sample(preset_number, ch, z);
        if (i >= 0) {
            if (samples[i].state != SMP_READY) {
                return 0;
            }
            any = 1;
        }
    }
    return any;
}

/* Fraction (0..1000) of a sampler channel's zone samples loaded, -1 when
 * it is ready, has none, or any failed. Read-only: the loader owns the
 * samples. */
static int channel_load_permille(int ch)
{
    uint32_t sum = 0;
    int n = 0;

    if (preset.ch[ch].machine) {
        return -1;
    }
    for (int z = 0; z < YML_ZONES; z++) {
        int i = storage_sample(preset_number, ch, z);
        if (i < 0) {
            continue;
        }
        const struct sample *sm = &samples[i];
        if (sm->state >= SMP_NOMEM) {
            return -1;
        }
        if (sm->state == SMP_READY) {
            sum += 1000;
        } else if (sm->state == SMP_LOADING && sm->frames) {
            uint32_t d = sm->loaded > sm->frames ? sm->frames : sm->loaded;
            sum += (uint32_t)((uint64_t)d * 1000u / sm->frames);
        }
        n++;
    }
    return n && sum < 1000u * (uint32_t)n ? (int)(sum / (uint32_t)n) : -1;
}

static void draw_channel_box(int x, int y, int ch, int big)
{
    char n[2] = { (char)('1' + ch), 0 };
    int has = channel_ready(ch);

    if (ch == (rview >= 0 ? rview : sel)) {
        gfx_rect(x, y - 1, 7, 9, C_RED);
        gfx_text(x + 1, y, n, C_BLACK);
    } else if (has && big) {
        gfx_rect(x, y - 1, 7, 9, C_DRED);
        gfx_text(x + 1, y, n, C_BLACK);
    } else {
        gfx_rect(x + 2, y + 3, 2, 2, C_RED);
    }
}

/* The left column of channel markers on parameter pages. */
static void draw_channel_column(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        draw_channel_box(0, 24 + ch * 13, ch, 0);
    }
}

/* Stereo/Right channel ch shows a branch from its Master instead of a
 * sample name (stock 2.0); 1 = the other side of the Master's samples,
 * 2 = it plays something else (the branch gets a slash), 0 = no branch. */
static int branch_kind(int ch)
{
    int n = zone_count(ch);

    if (ch < 1 || pair_of(ch - 1) != ch) {
        return 0;
    }
    if (!n || n != zone_count(ch - 1)) {
        return 2;
    }
    for (int z = 0; z < n; z++) {
        const struct zone_cfg *m = zcfg(ch - 1, z), *r = zcfg(ch, z);
        if (r->smp != m->smp || !sample_stereo(r->smp - 1) || r->side == m->side) {
            return 2;
        }
    }
    return 1;
}

static void draw_branch(int x, int y, int w, int kind, int focused, uint16_t c)
{
    if (focused) {
        gfx_rect(x - 1, y - 1, w + 2, 9, C_WHITE);
        c = C_BLACK;
    }
    for (int k = 0; k < 4; k++) {
        gfx_rect(x + 1 + k, y + k, 1, 1, c);
    }
    gfx_rect(x + 5, y + 4, w - 6, 1, c);
    if (kind == 2) {
        for (int k = 0; k < 4; k++) {
            gfx_rect(x + w - 4 + k, y + 6 - k, 1, 1, c);
        }
    }
}

static void draw_header(const char *title)
{
    draw_lr_meters();
    gfx_title(24, 2, title, C_TITLE);
}

static void draw_toast(void)
{
    if (toast[0] && toast_until > time_us64()) {
        int w = gfx_text_width(toast) + 6;
        gfx_rect(64 - w / 2, 56, w, 15, C_ORANGE);
        gfx_text(64 - w / 2 + 3, 60, toast, C_BLACK);
    }
}

static float dest_fill(int d, int v)
{
    int lo, hi;

    dest_range(d, &lo, &hi);
    return (float)(v - lo) / (float)(hi - lo);
}

/* A8 parameter block: name (and live value), value field, CV row. */
static void param_block(int y, int d, const char *title)
{
    char s[24];

    gfx_title(12, y, title, C_TITLE);
    fmt_dest(s, engine_value[rview >= 0 ? rview : sel][d], d);
    if (preset.ch[sel].mod[d].src != CV_OFF) {
        gfx_text_right(127, y, s, C_DIM);
    }
    field(12, y + 11, &f_dest, d, 0);
    if (d >= D_CTRL) {
        gfx_bar(80, y + 11, 46, 7, dest_fill(d, rview >= 0 ? rofs_value(d) : get_dest(d)), C_ORANGE);
    }
    field(12, y + 21, &f_src, d, 0);
    field(46, y + 21, &f_amt, d, 0);
    gfx_bar_bipolar(84, y + 22, 30, 5, (float)preset.ch[sel].mod[d].amt / AMT_MAX, C_GREEN);
}

/* Unsaved marker: a triangle, apex at (cx, y), `rows` high. */
static void draw_tri(int cx, int y, int rows, uint16_t c)
{
    for (int r = 0; r < rows; r++) {
        gfx_rect(cx - r, y + r, 2 * r + 1, 1, c);
    }
}

static const char *slot_label(int n)
{
    const char *name = storage_name(n);

    return name ? name : storage_state(n) == SLOT_PENDING ? "LOADING" : "~EMPTY~";
}

static void draw_home(void)
{
    char num[8], s[24];
    int shown = browse_active ? browse_num : preset_number;
    const char *name = preset.name;
    uint16_t color = C_WHITE;

    if (cancel_down && page == P_HOME) {
        uint32_t el = time_us() - cancel_at, left = el < LONG_PRESS_US ? LONG_PRESS_US - el : 0;
        s[0] = 0;
        cat(s, "CANCELLING IN ");
        put_uint(s + 14, (left + 999999u) / 1000000u, 1);
        cat(s, " SEC");
        gfx_text(0, 0, s, C_RED);
    } else {
        draw_lr_meters();
    }
    put_uint(num, (unsigned)shown, 3);
    if (browse_active) {
        name = slot_label(browse_num);
        color = C_YELLOW;
    } else if (storage_blank()) {
        name = "~EMPTY~";
    }
    gfx_text(0, 18, num, color);
    if (storage_changed(shown)) {
        draw_tri(19, 20, 3, C_YELLOW);
    }
    if (gfx_text_width(name) <= 8 * GFX_CHAR_W) {
        oled_text(22, 14, name, color, 2);
    } else {
        gfx_text(22, 18, name, color);
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        int y = 33 + ch * 11;
        const struct channel_cfg *c = &preset.ch[ch];
        static const uint16_t cvc[3] = { C_RED, C_GREEN, 0x3a7fu };
        static const uint16_t cvn[3] = { 0x7082u, 0x1b43u, 0x1931u };

        draw_channel_box(0, y, ch, 1);
        /* Zone Lock: the locked zone, flashing, in place of the CV bars (2.0). */
        if (engine_zone_lock[ch] && !c->machine) {
            char zn[2] = { (char)('0' + engine_zone_lock[ch]), 0 };
            if ((time_us() / 250000u) & 1u) {
                gfx_rect(10, y - 1, 7, 9, C_WHITE);
                gfx_text(11, y, zn, C_BLACK);
            }
        } else {
            for (int k = 0; k < 3; k++) {
                float v = cv_volts[ch * 3 + k];
                int h = (int)((v < 0 ? -v : v) * (8.0f / 5.0f) + 0.5f);
                gfx_rect(10 + k * 3, y + 7 - (h > 8 ? 8 : h), 2, h > 8 ? 8 : h ? h : 1, h ? (v < 0 ? cvn[k] : cvc[k]) : C_FAINT);
            }
        }
        if (branch_kind(ch)) {
            draw_branch(22, y, 38, branch_kind(ch), 0, channel_ready(ch) ? C_WHITE : C_DIM);
        } else if (c->machine || channel_sample(ch) < 0) {
            gfx_text(22, y, machine_name(c->machine), c->machine ? C_WHITE : C_DIM);
        } else {
            char nm[8], *dot;
            copy_n(nm, samples[channel_sample(ch)].name, 7);
            if ((dot = strchr(nm, '.'))) {
                *dot = 0;
            }
            gfx_text(22, y, nm, channel_ready(ch) ? C_WHITE : C_DIM);
        }
        if (storage_samples_percent() >= 0) {
            int lp = channel_load_permille(ch);
            if (lp >= 0) {
                gfx_rect(22, y + 9, 38, 1, C_FAINT);
                gfx_rect(22, y + 9, 38 * lp / 1000, 1, C_TITLE);
            }
        }
        if (c->machine) {
            int h = (int)(engine_status.peak[ch] * 8.0f + 0.5f);
            h = h > 8 ? 8 : h;
            gfx_rect(64, y + 7 - h, 3, h, C_GREEN);
        }
        if ((engine_status.gates >> ch) & 1) {
            gfx_rect(68, y + 1, 2, 5, C_YELLOW);
        }
    }
    /* A8: the selected channel's waveform, while it plays. */
    engine_scope_ch = sel;
    if ((engine_status.awake >> sel) & 1) {
        unsigned head = engine_scope_head;
        for (int i = 0; i < SCOPE_N; i++) {
            unsigned k = (head + (unsigned)i) % SCOPE_N;
            int lo = 75 - engine_scope_max[k] * 40 / 127, hi = 75 - engine_scope_min[k] * 40 / 127;
            gfx_rect(72 + i, lo, 1, hi - lo + 1, C_TITLE);
        }
    }
    if (storage_changed_count()) {
        draw_tri(3, 121, 4, C_YELLOW);
    }
    s[0] = 0;
    if (storage_pending() || storage_samples_percent() >= 0) {
        int pc = storage_pending() ? (storage_total() - storage_pending()) * 100 / storage_total()
                                   : storage_samples_percent();
        cat(s, "LOADING ");
        put_uint(s + 8, (unsigned)pc, 1);
        cat(s, "%");
    } else {
        copy_n(s, storage_folder()[0] ? storage_folder() : storage_card() ? "NO FOLDER" : "NO CARD",
               preset.d2cv ? 12 : 19);
    }
    gfx_text(10, 120, s, C_FAINT);
    if (preset.d2cv) {
        s[0] = 0;
        cat(s, "D2CV ");
        fmt_src(s + 5, CV_ABS + preset.d2cv - 1);
        gfx_text_right(127, 120, s, C_DIM);
    }
}

static void fmt_xfw(char *o, int v, int a)
{
    int w = xfw_value(v);

    (void)a;
    fmt_fixed(o, w < 100 ? w : w / 10, w < 100 ? 2 : 1, 1, 0);
    cat(o, "V");
}

static int marquee_off(int over, uint32_t since);

#define CH_NAME_W (8 * GFX_CHAR_W)

/* The cursor row's long sample name scrolls inside the 8-character column;
 * the L/R marker stays at its end. */
static void draw_chname(int x, int y, int ch, int row_cur)
{
    static int last_v = -1, last_f = -1;
    static uint32_t since;
    int v = get_zsmp(ch * ZONES);
    char nm[24], *dot, side[2] = { 0, 0 };

    if (v < 0 || !samples_valid(SMP_INDEX(v)) || !row_cur) {
        goto plain;
    }
    copy_n(nm, samples[SMP_INDEX(v)].name, 20);
    if ((dot = strchr(nm, '.'))) {
        *dot = 0;
    }
    if (gfx_text_width(nm) - 1 <= CH_NAME_W) {
        goto plain;
    }
    if (v != last_v || focus[P_CHANNELS] != last_f) {
        last_v = v;
        last_f = focus[P_CHANNELS];
        since = time_us();
    }
    if (sample_stereo(SMP_INDEX(v))) {
        side[0] = v & SMP_SIDE_R ? FONT_SUB_R[0] : FONT_SUB_L[0];
    }
    {
        int foc = field_reg(&f_chsmp, ch * ZONES), w = CH_NAME_W + (side[0] ? GFX_CHAR_W : 0);
        uint16_t c = foc ? C_BLACK : C_WHITE;

        if (foc) {
            gfx_rect(x - 1, y - 1, w + 1, FONT_H + 2, fine ? C_ORANGE : C_WHITE);
        }
        oled_clip_x(x, x + CH_NAME_W);
        gfx_text(x - marquee_off(gfx_text_width(nm) - 1 - CH_NAME_W, since), y, nm, c);
        oled_clip_x(0, OLED_W);
        if (side[0]) {
            gfx_text(x + CH_NAME_W, y, side, c);
        }
    }
    return;
plain:
    field(x, y, &f_chsmp, ch * ZONES, 0);
}

static void draw_channels(void)
{
    char t[4] = "XF";

    draw_header("CHANNELS");
    gfx_text(12, 14, "MACHINE", C_TITLE);
    gfx_text(68, 14, "MD", C_TITLE);
    gfx_text(98, 14, "X", C_TITLE);
    gfx_text_right(127, 14, "MIX", C_TITLE);
    if (focus[P_CHANNELS] < CHANNELS * XF_ROW_FIELDS) {
        xf_show = preset.ch[focus[P_CHANNELS] / XF_ROW_FIELDS].s.xfade - 1;
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        int y = 25 + ch * 11;
        draw_channel_box(0, y, ch, 1);
        if (branch_kind(ch)) {
            draw_branch(12, y, 48, branch_kind(ch), field_reg(&f_chsmp, ch * ZONES), C_WHITE);
        } else if (is_sampler(ch) && !chan_off(ch)) {
            draw_chname(12, y, ch, focus[P_CHANNELS] / XF_ROW_FIELDS == ch);
        } else {
            field(12, y, &f_machine, ch, 0);
        }
        field(68, y, &f_mode, ch, 0);
        field(98, y, &f_xgroup, ch, 0);
        field(0, y, &f_mix_on, ch, 127);
    }
    gfx_title(0, 117, "D2CV", C_TITLE);
    field(30, 117, &f_d2cv, 0, 0);
    gfx_text(52, 117, t, C_TITLE);
    field(64, 117, &f_xfsel, 0, 0);
    field_dim = xf_show < 0;
    field(74, 117, &f_xfcv, 0, 0);
    field(0, 117, &f_xfw, 0, 127);
    field_dim = 0;
}

#define OV_TOP  26
#define OV_BOT  122
#define SEC_H   11

static const char *const sec_name[] = { "PITCH", "ENVELOPE", "MUTATE", "PHASE MOD", "LEVEL" };

static struct ov_item {
    int16_t y;
    uint8_t p, x, tag, first;
} ov_it[CONTROLS];
static int ov_n, ov_cur[2];

/* Lays the voice's params out in sections in tag (button) order; returns
 * the content height. Content y is relative to OV_TOP. */
static int ov_build(const struct voice_desc *v, int per_row, int row_h)
{
    int y = 0;

    ov_n = 0;
    if (!v) {
        return 0;
    }
    for (int tag = TAG_PITCH; tag <= TAG_LEVEL; tag++) {
        int n = 0;

        for (int p = 0; p < (int)v->nparams && p < CONTROLS; p++) {
            if (v->params[p].tag != tag) {
                continue;
            }
            if (n == 0) {
                y += SEC_H;
            } else if (n % per_row == 0) {
                y += row_h;
            }
            ov_it[ov_n++] = (struct ov_item){ (int16_t)y, (uint8_t)p, (uint8_t)(12 + n % per_row * 58), (uint8_t)tag, n == 0 };
            n++;
        }
        if (n) {
            y += row_h;
        }
    }
    return y;
}

/* Eases *cur toward keeping the span [top, bot) in view (top < 0: scroll
 * to the start). */
static int scroll_to(int *cur, int total, int view, int top, int bot)
{
    int max = total > view ? total - view : 0, t = *cur, d;

    if (top < 0) {
        t = 0;
    } else {
        t = top < t ? top : bot > t + view ? bot - view : t;
    }
    t = t < 0 ? 0 : t > max ? max : t;
    d = t - *cur;
    *cur += d > 0 ? (d + 1) / 2 : d < 0 ? (d - 1) / 2 : 0;
    return *cur;
}

static int ov_scroll(int slot, int total, int foc, int row_h)
{
    if (foc < 0) {
        return scroll_to(&ov_cur[slot], total, OV_BOT - OV_TOP, -1, 0);
    }
    return scroll_to(&ov_cur[slot], total, OV_BOT - OV_TOP, ov_it[foc].y - (ov_it[foc].first ? SEC_H : 0) - 2, ov_it[foc].y + row_h + 2);
}

static void ov_arrow(int x, int y, int down)
{
    for (int i = 0; i < 4; i++) {
        gfx_rect(x - i, down ? y + 3 - i : y + i, 2 * i + 1, 1, C_DIM);
    }
}

static void ov_end(int total, int scroll, int view)
{
    gfx_clip_y(0, OLED_H);
    if (scroll > 0) {
        ov_arrow(121, 16, 0);
    }
    if (scroll < total - view) {
        ov_arrow(64, 123, 1);
    }
}

static void ov_section(const struct ov_item *it, int sy)
{
    int w = gfx_text(12, sy, sec_name[it->tag], C_TITLE);

    gfx_rect(w + 3, sy + 4, 126 - w - 3, 1, C_FAINT);
}

static void draw_machine(void)
{
    char s[8];
    const struct voice_desc *v = voices[preset.ch[sel].machine];
    int total = ov_build(v, 2, 24), foc = focus[P_MACHINE] - 1, sc;

    draw_header("MACHINE");
    draw_channel_column();
    field(12, 14, &f_machine, sel, 0);
    sc = ov_scroll(0, total, foc < ov_n ? foc : -1, 24);
    gfx_clip_y(OV_TOP, OV_BOT);
    for (int k = 0; k < ov_n; k++) {
        const struct ov_item *it = &ov_it[k];
        int x = it->x, y = OV_TOP + it->y - sc, d = D_CTRL + it->p;
        const char *name = v->params[it->p].name;
        int a = d, vc = rview >= 0 ? rview : sel;
        const struct field *f = rofs_sub(&f_dest, &a);
        int foc_k = field_reg(f, a), bar = 50;

        if (it->first) {
            ov_section(it, y - SEC_H);
        }
        gfx_frame(x + 1, y, 56, 22, foc_k ? (fine ? C_ORANGE : C_WHITE) : C_DIM);
        if (foc_k) {
            gfx_text_box(x + 4, y + 3, name, C_BLACK, fine ? C_ORANGE : C_WHITE);
        } else {
            gfx_text(x + 4, y + 3, name, C_TITLE);
        }
        if (preset.ch[sel].mod[d].src != CV_OFF) {
            gfx_text(x + 26, y + 3, "~", C_GREEN);
        }
        fmt_dest(s, engine_value[vc][d], d);
        gfx_text_right(x + 54, y + 3, s, C_WHITE);
        if (f != &f_dest && preset.ch[rview].ofs[a]) {
            bar = 34;
            rofs_mark(x + 40, y + 14, a);
        }
        gfx_bar(x + 4, y + 14, bar, 5, (float)engine_value[vc][d] / 127.0f, C_ORANGE);
    }
    ov_end(total, sc, OV_BOT - OV_TOP);
    if (!v) {
        gfx_text(12, 60, "NO MACHINE", C_DIM);
    }
}

static void draw_machine_cv(void)
{
    const struct voice_desc *v = voices[preset.ch[sel].machine];
    int total = ov_build(v, 1, 12), foc = (focus[P_MACHINE_CV]) / 2, sc;

    draw_header("MACHINE CV");
    draw_channel_column();
    gfx_text(12, 15, machine_name(preset.ch[sel].machine), C_DIM);
    sc = ov_scroll(1, total, foc < ov_n ? foc : -1, 12);
    gfx_clip_y(OV_TOP, OV_BOT);
    for (int k = 0; k < ov_n; k++) {
        const struct ov_item *it = &ov_it[k];
        int y = OV_TOP + it->y - sc, d = D_CTRL + it->p;

        if (it->first) {
            ov_section(it, y - SEC_H);
        }
        gfx_text(12, y, v->params[it->p].name, C_TITLE);
        field(40, y, &f_src, d, 0);
        field(70, y, &f_amt, d, 0);
        gfx_bar_bipolar(104, y + 1, 23, 5, (float)preset.ch[sel].mod[d].amt / AMT_MAX, C_GREEN);
    }
    ov_end(total, sc, OV_BOT - OV_TOP);
}

#define PG_TOP 14
#define PG_BLOCK_H 34

static struct {
    int y, ftop, fbot, sc;
} pg;

static void pg_begin(void)
{
    pg.y = 0;
    pg.ftop = -1;
    gfx_clip_y(PG_TOP, OV_BOT);
}

static void pg_item(int first, int h)
{
    if (focus[page] >= first && focus[page] < nfields) {
        pg.ftop = pg.y;
        pg.fbot = pg.y + h;
    }
    pg.y += h;
}

static void pg_block(const char *title, int d)
{
    int first = nfields;

    param_block(PG_TOP + 4 + pg.y - pg.sc, d, title);
    pg_item(first, PG_BLOCK_H);
}

static void pg_end(void)
{
    int view = OV_BOT - PG_TOP;

    pg.sc = scroll_to(&pg.sc, pg.y, view, pg.ftop - 2, pg.fbot + 2);
    ov_end(pg.y, pg.sc, view);
}

/* Machine button page: `own` draws the channel's controls, then come the
 * voice's params tagged for the page; returns how many. */
static int draw_mpage(const char *title, int tag, void (*own)(void))
{
    const struct voice_desc *v = voices[preset.ch[sel].machine];
    int n = 0;

    draw_header(title);
    draw_channel_column();
    pg_begin();
    if (own) {
        own();
    }
    for (int p = 0; v && p < (int)v->nparams && p < CONTROLS; p++) {
        if (v->params[p].tag == tag) {
            pg_block(v->params[p].name, D_CTRL + p);
            n++;
        }
    }
    pg_end();
    return n;
}

static void own_pitch(void) { pg_block("PITCH", D_PITCH); }
static void own_level(void) { pg_block("LEVEL", D_LEVEL); }
static void sroute(int y, int i);

/* A channel value with its CV route, sampler-page style. */
static void pg_sval(const char *title, int a, int route, const struct field *extra)
{
    int first = nfields, y = PG_TOP + 4 + pg.y - pg.sc;

    gfx_title(12, y, title, C_TITLE);
    field(12, y + 10, &f_sval, a, 0);
    if (extra) {
        /* the right side's view: on the title line, clear of the offset */
        field(0, rview >= 0 ? y : y + 10, extra, 0, 127);
    }
    sroute(y + 20, route);
    pg_item(first, PG_BLOCK_H);
}

static void own_mutate(void)
{
    pg_block("BIT DEPTH", D_BITS);
    pg_sval("ALIASING", SV_ALIAS, SM_ALIAS, 0);
}

static void own_env(void)
{
    int first, y;

    pg_sval("START DELAY", SV_DELAY, SM_DELAY, 0);
    pg_sval("ATTACK", SV_ATTACK, SM_ATTACK, &f_afc);
    pg_sval("RELEASE", SV_RELEASE, SM_RELEASE, 0);
    first = nfields;
    y = PG_TOP + 4 + pg.y - pg.sc;
    gfx_title(12, y, "PLAY", C_TITLE);
    gfx_title(72, y, "TRIG", C_TITLE);
    field(12, y + 10, &f_play, 0, 0);
    field(0, y + 10, &f_auto, sel, 127);
    pg_item(first, 24);
}

static void draw_mod_page(void)
{
    if (!draw_mpage("PHASE MOD", TAG_MOD, 0)) {
        gfx_text(12, 30, "NO MOD PARAMS", C_DIM);
        gfx_text(12, 44, machine_name(preset.ch[sel].machine), C_WHITE);
    }
}

static void draw_sampler_only(void)
{
    draw_header(sampler_only_title);
    draw_channel_column();
    if (is_sampler(sel)) {
        gfx_text(12, 30, "NOT YET", C_DIM);
        return;
    }
    gfx_text(12, 30, "SAMPLER ONLY", C_DIM);
    gfx_text(12, 44, "CHANNEL RUNS", C_DIM);
    gfx_text(12, 54, machine_name(preset.ch[sel].machine), C_WHITE);
}

/* ---- Sampler pages ---- */

static int blink_on(void)
{
    return (time_us() / 250000u) & 1u;
}

/* Selected zone and (red) the zone last played, top right; the selected
 * number flashes while it is locked (2.0). */
static void draw_zone_ind(void)
{
    char s[4] = { 'Z', (char)('1' + cur_zone()), ':', 0 }, p[2] = { (char)('1' + engine_status.zone[sel]), 0 };

    if (!zone_count(sel)) {
        return;
    }
    if (!engine_zone_lock[sel] || blink_on()) {
        gfx_text(104, 2, s, C_WHITE);
    }
    gfx_text(122, 2, p, C_RED);
}

static void route_row(int y, const struct field *src, const struct field *amt, int arg, int amount)
{
    field(12, y, src, arg, 0);
    field(40, y, amt, arg, 0);
    gfx_bar_bipolar(76, y + 1, 30, 5, (float)amount / 100.0f, C_GREEN);
}

static void droute(int y, int d)
{
    route_row(y, &f_src, &f_amt1, d, preset.ch[sel].mod[d].amt);
}

static void sroute(int y, int i)
{
    route_row(y, &f_ssrc, &f_samt, i, preset.ch[sel].s.mod[i].amt);
}

static void live(int y, int d, int v)
{
    char s[16];

    if (preset.ch[sel].mod[d].src != CV_OFF) {
        fmt_sval(s, v, d == D_PITCH ? SV_PITCH : SV_LEVEL);
        s[6] = 0;
        gfx_text_right(127, y, s, C_DIM);
    }
}

static void spage(const char *title, int zones)
{
    draw_header(title);
    draw_channel_column();
    if (zones) {
        draw_zone_ind();
    }
}

static void draw_pitch_s(void)
{
    spage("PITCH", 1);
    field(12, 15, &f_sval, SV_PITCH, 0);
    live(15, D_PITCH, engine_value[rview >= 0 ? rview : sel][D_PITCH]);
    droute(25, D_PITCH);
    if (zone_count(sel) > 1) {
        gfx_title(12, 37, "ZONE OFFSET", C_TITLE);
        field(12, 47, &f_sval, SV_ZPITCH, 0);
    }
    gfx_title(12, 61, "LINEAR FM", C_TITLE);
    sroute(71, SM_LINFM);
    gfx_title(12, 85, "EXP FM", C_TITLE);
    sroute(95, SM_EXPFM);
}

static void draw_level_s(void)
{
    spage("LEVEL", 1);
    field(12, 15, &f_sval, SV_LEVEL, 0);
    live(15, D_LEVEL, engine_value[rview >= 0 ? rview : sel][D_LEVEL]);
    if (zone_count(sel) > 1) {
        gfx_title(12, 29, "ZONE OFFSET", C_TITLE);
        field(12, 39, &f_sval, SV_ZLEVEL, 0);
    }
    gfx_title(12, 55, "LINEAR AM", C_TITLE);
    sroute(65, SM_LINAM);
    field(0, 55, &f_extenv, 0, 127);
    gfx_title(12, 81, "EXP AM", C_TITLE);
    sroute(91, SM_EXPAM);
}

static void draw_mutate_s(void)
{
    spage("MUTATE", 0);
    gfx_title(12, 14, "BIT DEPTH", C_TITLE);
    field(12, 24, &f_dest, D_BITS, 0);
    droute(34, D_BITS);
    gfx_title(12, 48, "ALIASING", C_TITLE);
    field(12, 58, &f_sval, SV_ALIAS, 0);
    sroute(68, SM_ALIAS);
    gfx_title(12, 82, "REVERSE", C_TITLE);
    field(12, 92, &f_rev, 0, 0);
    gfx_title(72, 82, "REV CV", C_TITLE);
    field(72, 92, &f_rcv, 0, 0);
    gfx_title(12, 106, "SMOOTH", C_TITLE);
    field(12, 116, &f_smooth, 0, 0);
}

/* PHASE MOD (stock layout): source (a channel, a sampling input or a CV
 * whose route is grayed out otherwise), index and its CV route. */
static void draw_phase_s(void)
{
    const struct sampler_cfg *sc = &preset.ch[sel].s;
    char t[16];

    spage("PHASE MOD", 0);
    gfx_title(12, 14, "SOURCE", C_TITLE);
    field(12, 24, &f_pmsrc, 0, 0);
    field_dim = sc->pm_source != PM_CV;
    sroute(36, SM_PHASE);
    field_dim = 0;
    gfx_title(12, 54, "INDEX", C_TITLE);
    field(12, 64, &f_sval, SV_PMINDEX, 0);
    int pmi = sc->pm_index, o = rview >= 0 ? preset.ch[rview].ofs[PO_PMINDEX] : 0;
    fmt_fixed(t, preset_offset_apply(PO_PMINDEX, 0, pmi, o) * 256 / 10, 1, 1, 0);
    cat(t, " SMPLS");
    gfx_text(o ? 66 : 52, 64, t, C_FAINT);
    sroute(76, SM_PMINDEX);
}

/* A Stereo/Right channel keeps its own pan, its mix level is an offset
 * from its master's; the rest is the master's. */
static int sr_note(int y)
{
    char t[24] = "CV: AS CH ";
    int m = preset_param_channel(sel);

    if (m == sel) {
        return 0;
    }
    gfx_title(12, y, "MIX LEVEL", C_TITLE);
    field(12, y + 10, &f_dest, D_MIX, 0);
    t[10] = (char)('1' + m);
    t[11] = 0;
    gfx_text(12, y + 22, t, C_FAINT);
    return 1;
}

static void draw_panmix_s(void)
{
    spage("PAN/MIX", 0);
    gfx_title(12, 14, "PAN", C_TITLE);
    field(12, 24, &f_dest, D_PAN, 0);
    gfx_bar_bipolar(56, 25, 50, 6, (float)engine_value[sel][D_PAN] / 100.0f, C_YELLOW);
    droute(34, D_PAN);
    if (sr_note(48)) {
        return;
    }
    gfx_title(12, 48, "MIX LEVEL", C_TITLE);
    field(12, 58, &f_dest, D_MIX, 0);
    droute(68, D_MIX);
    field(0, 58, &f_fader, 0, 127);
    gfx_title(12, 84, "IN MIX", C_TITLE);
    field(70, 84, &f_mix_on, sel, 0);
}

static void draw_env_s(void)
{
    int first, y;

    spage("ENVELOPE", 0);
    pg_begin();
    pg_sval("START DELAY", SV_DELAY, SM_DELAY, 0);
    pg_sval("ATTACK", SV_ATTACK, SM_ATTACK, &f_afc);
    pg_sval("RELEASE", SV_RELEASE, SM_RELEASE, 0);
    first = nfields;
    y = PG_TOP + 4 + pg.y - pg.sc;
    gfx_title(12, y, "PLAY", C_TITLE);
    gfx_title(52, y, "LOOP", C_TITLE);
    gfx_title(98, y, "TRIG", C_TITLE);
    field(12, y + 10, &f_play, 0, 0);
    field(52, y + 10, &f_loop, 0, 0);
    field(0, y + 10, &f_auto, sel, 127);
    pg_item(first, 24);
    pg_end();
}

/* ---- EQ, EQ CV and Filter (Tallent Edition pages, our look) ----
 * MUTATE -> EQ -> EQ CV -> MUTATE, ENVELOPE -> FILTER -> ENVELOPE. EDIT
 * picks the channel's settings or the selected zone's own (sample channels);
 * a zone follows its channel (values gray) until a value is changed. Hold
 * on EQ / TYPE resets the page (CV routes kept), hold again undoes; hold on
 * EDIT makes the zone follow again. FILTER on a Stereo/Right channel sets
 * the right side's cutoff, an offset from the master's. */

static int fx_zone_edit;
static struct eq_value eq_held;
static struct filter_value filt_held;
static int eq_held_owner, filt_held_owner;

static int fx_ch(void) { return preset_param_channel(sel); }
static int fx_zoned(void) { return fx_zone_edit && is_sampler(fx_ch()); }
static struct zone_cfg *fx_zone(void) { return zcfg(fx_ch(), cur_zone()); }
static int fx_owner(void) { return 1 + fx_ch() * 9 + (fx_zoned() ? cur_zone() : 8); }

static struct eq_value *eq_cur(void)
{
    return fx_zoned() && fx_zone()->eq.override ? &fx_zone()->eq : &preset.ch[fx_ch()].eq;
}

static struct filter_value *filt_cur(void)
{
    return fx_zoned() && fx_zone()->filter.override ? &fx_zone()->filter : &preset.ch[fx_ch()].filter;
}

static int eq_following(void) { return fx_zoned() && !fx_zone()->eq.override; }
static int filt_following(void) { return fx_zoned() && !fx_zone()->filter.override; }
static int filt_right(void) { return sel != fx_ch(); }
static int filt_paired(void) { int m = fx_ch(); return m + 1 < CHANNELS && preset.ch[m + 1].mode == MODE_STEREO; }

static void eq_apply(struct eq_value *v)
{
    eq_limit(v);
    if (!memcmp(v, eq_cur(), sizeof(*v))) {
        return;
    }
    if (fx_zoned()) {
        v->override = 1;
        fx_zone()->eq = *v;
    } else {
        preset.ch[fx_ch()].eq = *v;
    }
}

static void filt_apply(struct filter_value *v)
{
    if (!memcmp(v, filt_cur(), sizeof(*v))) {
        return;
    }
    if (fx_zoned()) {
        v->override = 1;
        fx_zone()->filter = *v;
    } else {
        preset.ch[fx_ch()].filter = *v;
    }
}

/* Encoder held while turning in an edit: large steps (Tallent's
 * panel_step: onto the grid from anchor, then whole grid steps). */
static int coarse_step(int v, int steps, int grid, int anchor)
{
    int o = v - anchor, below = o >= 0 ? o / grid * grid : -((grid - 1 - o) / grid * grid);

    if (steps < 0 && below != o) {
        below += grid;
    }
    return anchor + below + steps * grid;
}

/* A frequency step as the pages show it: tenths of a hertz below 200 Hz,
 * whole hertz, then kHz to thousandths, above 10 kHz hundredths. */
static void fmt_step(char *o, int step, int unit)
{
    unsigned c = eq_step_centihertz((u32)step);

    if (c < 20000u) {
        fmt_fixed(o, (int)((c + 5) / 10), 1, 1, 0);
    } else if (c < 100000u) {
        fmt_int(o, (int)((c + 50) / 100));
    } else if (c < 1000000u) {
        fmt_fixed(o, (int)((c + 50) / 100), 3, 1, 0);
    } else {
        fmt_fixed(o, (int)((c + 500) / 1000), 2, 1, 0);
    }
    if (c >= 100000u) {
        cat(o, "K");
    } else if (unit) {
        cat(o, "HZ");
    }
}

/* EQ page fields; arg = band. */
static int get_eqf(int b) { return eq_cur()->frequency[b]; }
static void set_eqf(int b, int v) { struct eq_value n = *eq_cur(); n.frequency[b] = (uint16_t)v; eq_apply(&n); }
static void range_eqf(int b, int *lo, int *hi) { (void)b; *lo = 0; *hi = EQ_STEP_MAX; }
static void fmt_eqf(char *o, int v, int b) { (void)b; fmt_step(o, v, 0); }
static int coarse_freq(int a, int v, int s) { (void)a; return coarse_step(v, s, EQ_STEPS_PER_OCTAVE / 12, EQ_STEP_1K); }
static const struct field f_eqf = { get_eqf, set_eqf, range_eqf, fmt_eqf, 0, L_NONE, 0, 0 };

static int get_eqg(int b) { return eq_cur()->gain[b]; }
static void set_eqg(int b, int v) { struct eq_value n = *eq_cur(); n.gain[b] = (int16_t)v; eq_apply(&n); }
static void range_eqg(int b, int *lo, int *hi) { *hi = eq_gain_limit(eq_cur(), (u32)b); *lo = -*hi; }
static void fmt_eqg(char *o, int v, int b) { (void)b; fmt_fixed(o, v, 1, 1, 1); }
static int coarse_ten(int a, int v, int s) { (void)a; return coarse_step(v, s, 10, 0); }
static const struct field f_eqg = { get_eqg, set_eqg, range_eqg, fmt_eqg, 0, L_NONE, 0, 0 };

/* ---- A Stereo/Right channel's offsets ----
 * On its view of the parameter pages (rview) and on its Pan/Mix page, the
 * fields of values it plays as offsets (enum pofs) are proxies: they show
 * clamp(master + offset) with the offset in small dim figures, and an
 * edit sets the offset. The master's own field functions run with sel
 * on the master. Hold DATA1: the offset back to 0. */
static struct field_ref rofs_of[PO_COUNT];

static int rofs_right(void)
{
    if (rview >= 0) {
        return rview;
    }
    return page == P_PANMIX && preset_param_channel(sel) != sel ? sel : -1;
}

static int rofs_kind(const struct field *f, int arg)
{
    if (f == &f_dest) {
        return arg == D_PITCH ? PO_PITCH : arg == D_LEVEL ? PO_LEVEL : arg == D_MIX ? PO_MIX :
               arg == D_BITS ? PO_BITS : arg >= D_CTRL ? PO_CTRL + arg - D_CTRL : -1;
    }
    if (f == &f_sval) {
        return arg == SV_PITCH ? PO_PITCH : arg == SV_LEVEL ? PO_LEVEL : arg == SV_ATTACK ? PO_ATTACK :
               arg == SV_RELEASE ? PO_RELEASE : arg == SV_ALIAS ? PO_ALIAS : arg == SV_PMINDEX ? PO_PMINDEX :
               arg == SV_DELAY ? PO_DELAY : -1;
    }
    return f == &f_eqg ? PO_EQGAIN + arg : -1;
}

/* Runs the master's field function: sel on the master meanwhile. */
static int rofs_master_get(int k)
{
    int s = sel, v;

    sel = preset_param_channel(rofs_right());
    v = rofs_of[k].f->get(rofs_of[k].arg);
    sel = s;
    return v;
}

static int rofs_machine(void)
{
    return preset.ch[preset_param_channel(rofs_right())].machine != 0;
}

static int16_t *rofs_slot(int k)
{
    return &preset.ch[rofs_right()].ofs[k];
}

static int rofs_get(int k)
{
    if (k == PO_DELAY) {
        int t = preset.ch[preset_param_channel(rofs_right())].s.start_delay;
        return sdelay_index(preset_offset_apply(k, 0, t, *rofs_slot(k)));
    }
    return preset_offset_apply(k, rofs_machine(), rofs_master_get(k), *rofs_slot(k));
}

static void rofs_set(int k, int v)
{
    int lo, hi, o;

    if (k == PO_DELAY) {
        o = sdelay_time(v) - preset.ch[preset_param_channel(rofs_right())].s.start_delay;
    } else {
        o = v - rofs_master_get(k);
    }
    preset_offset_range(k, rofs_machine(), &lo, &hi);
    o = o < lo - hi ? lo - hi : o > hi - lo ? hi - lo : o;
    *rofs_slot(k) = (int16_t)o;
}

static void rofs_range(int k, int *lo, int *hi)
{
    int s = sel;

    sel = preset_param_channel(rofs_right());
    rofs_of[k].f->range(rofs_of[k].arg, lo, hi);
    sel = s;
}

static void rofs_fmt(char *o, int v, int k)
{
    int s = sel;

    if (k == PO_DELAY && v == rofs_get(k)) {
        int t = preset.ch[preset_param_channel(rofs_right())].s.start_delay;
        fmt_sdelay(o, preset_offset_apply(k, 0, t, *rofs_slot(k)));
        return;
    }
    sel = preset_param_channel(rofs_right());
    rofs_of[k].f->fmt(o, v, rofs_of[k].arg);
    sel = s;
}

static void rofs_hold(int k)
{
    *rofs_slot(k) = 0;
    ui_say("OFFSET RESET");
}

static const struct field f_rofs = { rofs_get, rofs_set, rofs_range, rofs_fmt, 0, L_NONE, 0, rofs_hold };

static const struct field *rofs_sub(const struct field *f, int *arg)
{
    int k = rofs_right() >= 0 ? rofs_kind(f, *arg) : -1;

    if (k < 0) {
        return f;
    }
    rofs_of[k].f = f;
    rofs_of[k].arg = *arg;
    *arg = k;
    return &f_rofs;
}

static int rofs_pitch(const struct field_ref *r)
{
    return r->f == &f_rofs && r->arg == PO_PITCH;
}

static const struct param_choice *rofs_choice(const struct field_ref *r)
{
    return r->f == &f_rofs && r->arg >= PO_CTRL ? dest_choice(D_CTRL + r->arg - PO_CTRL) : 0;
}

/* The value a channel destination plays on the right side's view. */
static int rofs_value(int d)
{
    int k = rofs_kind(&f_dest, d);

    if (k < 0) {
        return get_dest(d);
    }
    rofs_of[k].f = &f_dest;
    rofs_of[k].arg = d;
    return rofs_get(k);
}

/* 3x5 figures for the offsets: rows top down, 3 bits each. */
static void tiny_text(int x, int y, const char *t, uint16_t c)
{
    static const uint16_t g[] = {
        075557, 026227, 071747, 071717, 055711, 074717, 074757, 071122, 075757, 075717,
    };

    for (; *t; t++) {
        unsigned bits = *t >= '0' && *t <= '9' ? g[*t - '0'] : *t == '+' ? 002720u : *t == '-' ? 000700u : 0u;

        if (*t == '.') {
            gfx_rect(x, y + 4, 1, 1, c);
            x += 2;
            continue;
        }
        for (int r = 0; r < 5; r++) {
            for (int b = 0; b < 3; b++) {
                if ((bits >> (3 * (4 - r) + 2 - b)) & 1) {
                    gfx_rect(x + b, y + r, 1, 1, c);
                }
            }
        }
        x += 4;
    }
}

static void rofs_mark(int x, int y, int k)
{
    static const uint8_t dec[PO_COUNT] = { 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1 };
    int o = preset.ch[rofs_right()].ofs[k];
    char t[12];

    if (!o) {
        return;
    }
    fmt_fixed(t, o, dec[k], 1, 1);
    tiny_text(x, y, t, C_DIM);
}

static int get_eqq(int b) { return eq_cur()->q[b]; }
static void set_eqq(int b, int v) { struct eq_value n = *eq_cur(); n.q[b] = (uint8_t)v; eq_apply(&n); }
static void range_eqq(int b, int *lo, int *hi) { (void)b; *lo = 0; *hi = EQ_Q_STEPS; }
static void fmt_eqq(char *o, int v, int b)
{
    if (!v) {
        o[0] = 0;
        cat(o, b == EQ_BAND_MID ? "AUTO" : "SHELF");
        return;
    }
    int q = (int)eq_q_hundredths((u32)v);

    o[0] = 'Q';
    if (q < 1000) {
        fmt_fixed(o + 1, q, 2, 1, 0);
    } else {
        fmt_fixed(o + 1, q / 10, 1, 1, 0);
    }
}
static int coarse_q(int a, int v, int s) { (void)a; return coarse_step(v, s, EQ_Q_STEPS_PER_OCTAVE, 1); }
static const struct field f_eqq = { get_eqq, set_eqq, range_eqq, fmt_eqq, 0, L_NONE, 0, 0 };

/* HPF: 0 Off, else 1 + Hz - 20. */
static int get_eqhpf(int a) { (void)a; int h = eq_cur()->hpf; return h ? h - (int)EQ_HPF_MIN + 1 : 0; }
static void set_eqhpf(int a, int v)
{
    struct eq_value n = *eq_cur();

    (void)a;
    n.hpf = (uint16_t)(v ? v + (int)EQ_HPF_MIN - 1 : 0);
    eq_apply(&n);
}
static void range_eqhpf(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = EQ_HPF_MAX - EQ_HPF_MIN + 1; }
static void fmt_eqhpf(char *o, int v, int a)
{
    (void)a;
    if (!v) {
        memcpy(o, "OFF", 4);
        return;
    }
    fmt_int(o, v + (int)EQ_HPF_MIN - 1);
    cat(o, "HZ");
}
static int coarse_hpf(int a, int v, int s) { (void)a; return coarse_step(v, s, 10, 1); }
static const struct field f_eqhpf = { get_eqhpf, set_eqhpf, range_eqhpf, fmt_eqhpf, 0, L_NONE, 0, 0 };

/* Hold on EQ: flat (CV routes kept); again: back. */
static void eq_flat(struct eq_value *n)
{
    const struct eq_value *v = eq_cur();

    eq_value_defaults(n);
    memcpy(n->source, v->source, sizeof(n->source));
    memcpy(n->amount, v->amount, sizeof(n->amount));
    n->override = v->override;
}

static void hold_eqon(int a)
{
    struct eq_value n;

    (void)a;
    if (eq_following()) {
        return;
    }
    eq_flat(&n);
    if (memcmp(&n, eq_cur(), sizeof(n))) {
        eq_held = *eq_cur();
        eq_held_owner = fx_owner();
        eq_apply(&n);
        ui_say("EQ CLEARED");
    } else if (eq_held_owner == fx_owner()) {
        n = eq_held;
        memcpy(n.source, eq_cur()->source, sizeof(n.source));
        memcpy(n.amount, eq_cur()->amount, sizeof(n.amount));
        eq_held_owner = 0;
        eq_apply(&n);
        ui_say("EQ RESTORED");
    }
}

static int get_eqon(int a) { (void)a; return eq_cur()->on; }
static void set_eqon(int a, int v) { struct eq_value n = *eq_cur(); (void)a; n.on = (uint8_t)v; eq_apply(&n); }
static const struct field f_eqon = { get_eqon, set_eqon, range_onoff, fmt_onoff, 0, L_VLIST, 0, hold_eqon };

/* EDIT: CHAN / ZONE; hold: the zone follows its channel again. */
static int get_scope(int a) { (void)a; return fx_zone_edit; }
static void set_scope(int a, int v) { (void)a; fx_zone_edit = v; }
static void act_scope(int a) { (void)a; fx_zone_edit = !fx_zone_edit; }
static void fmt_scope(char *o, int v, int a) { (void)a; memcpy(o, v ? "ZONE" : "CHAN", 5); }
static void hold_eqscope(int a)
{
    (void)a;
    if (fx_zoned() && fx_zone()->eq.override) {
        memset(&fx_zone()->eq, 0, sizeof(fx_zone()->eq));
        if (eq_held_owner == fx_owner()) {
            eq_held_owner = 0;
        }
        ui_say("ZONE FOLLOWS CHANNEL");
    }
}
static void hold_fscope(int a)
{
    (void)a;
    if (fx_zoned() && fx_zone()->filter.override) {
        memset(&fx_zone()->filter, 0, sizeof(fx_zone()->filter));
        if (filt_held_owner == fx_owner()) {
            filt_held_owner = 0;
        }
        ui_say("ZONE FOLLOWS CHANNEL");
    }
}
static const struct field f_eqscope = { get_scope, set_scope, range_onoff, fmt_scope, act_scope, L_NONE, 0, hold_eqscope };
static const struct field f_fscope = { get_scope, set_scope, range_onoff, fmt_scope, act_scope, L_NONE, 0, hold_fscope };

/* CV routes: source as our CV enum (Off, CV A-C, 1A-8C = Tallent's -4..23
 * plus 4), amount -1.00..+1.00. arg = route. */
static int8_t *eq_src(int m, int amt) { return amt ? &eq_cur()->amount[m] : &eq_cur()->source[m]; }
static int get_eqsrc(int m) { return *eq_src(m, 0) - TA_SRC_OFF; }
static void set_eqsrc(int m, int v) { struct eq_value n = *eq_cur(); n.source[m] = (int8_t)(v + TA_SRC_OFF); eq_apply(&n); }
static int get_eqamt(int m) { return *eq_src(m, 1); }
static void set_eqamt(int m, int v) { struct eq_value n = *eq_cur(); n.amount[m] = (int8_t)v; eq_apply(&n); }
static const struct field f_eqsrc = { get_eqsrc, set_eqsrc, range_src, fmt_src_field, 0, L_CVSRC, 0, 0 };
static const struct field f_eqamt = { get_eqamt, set_eqamt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

static int get_fsrc(int m) { return filt_cur()->source[m] - TA_SRC_OFF; }
static void set_fsrc(int m, int v) { struct filter_value n = *filt_cur(); n.source[m] = (int8_t)(v + TA_SRC_OFF); filt_apply(&n); }
static int get_famt(int m) { return filt_cur()->amount[m]; }
static void set_famt(int m, int v) { struct filter_value n = *filt_cur(); n.amount[m] = (int8_t)v; filt_apply(&n); }
/* Two routes to a row: CV A-C as CVA-CVC. */
static void fmt_src_short(char *o, int v, int a)
{
    (void)a;
    fmt_src(o, v);
    if (v >= CV_REL_A && v <= CV_REL_C) {
        o[2] = o[3];
        o[3] = 0;
    }
}
static const struct field f_fsrc = { get_fsrc, set_fsrc, range_src, fmt_src_short, 0, L_CVSRC, 0, 0 };
static const struct field f_famt = { get_famt, set_famt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

/* Filter values; arg = FILTER_MOD_* of the setting. */
static const char *const ftype_labels[FILTER_TYPES] = {
    "OFF", "LP 24", "LP 12", "BP 24", "BP 12", "HP 24", "HP 12", "NOTCH 24", "NOTCH 12", "BAND STOP", "DUAL L/BP",
};

static int get_fval(int m)
{
    const struct filter_value *v = filt_cur();

    switch (m) {
    case FILTER_MOD_TYPE: return v->type;
    case FILTER_MOD_CUTOFF: return filt_right() ? (int)filter_right_cutoff(v) : v->cutoff;
    case FILTER_MOD_RESONANCE: return v->resonance;
    default: return v->extra;
    }
}

static void set_fval(int m, int x)
{
    struct filter_value n = *filt_cur();

    switch (m) {
    case FILTER_MOD_TYPE: n.type = (uint8_t)x; break;
    case FILTER_MOD_CUTOFF:
        if (filt_right()) {
            n.right = (int16_t)(x - n.cutoff);
        } else {
            n.cutoff = (uint16_t)x;
        }
        break;
    case FILTER_MOD_RESONANCE: n.resonance = (uint8_t)x; break;
    default: n.extra = (uint8_t)x; break;
    }
    filt_apply(&n);
}

static void range_fval(int m, int *lo, int *hi)
{
    *lo = 0;
    *hi = m == FILTER_MOD_TYPE ? FILTER_TYPES - 1 : m == FILTER_MOD_CUTOFF ? EQ_STEP_MAX : 100;
}

static void fmt_fval(char *o, int v, int m)
{
    if (m == FILTER_MOD_TYPE) {
        o[0] = 0;
        cat(o, ftype_labels[v]);
    } else if (m == FILTER_MOD_CUTOFF) {
        fmt_step(o, v, 1);
    } else {
        fmt_int(o, v);
    }
}

static int coarse_fval(int m, int v, int s)
{
    return m == FILTER_MOD_CUTOFF ? coarse_step(v, s, EQ_STEPS_PER_OCTAVE / 12, EQ_STEP_1K) :
           m == FILTER_MOD_TYPE ? v + s : coarse_step(v, s, 10, 0);
}

static void hold_ftype(int a)
{
    struct filter_value n;
    const struct filter_value *v = filt_cur();

    (void)a;
    if (filt_following()) {
        return;
    }
    filter_value_defaults(&n);
    memcpy(n.source, v->source, sizeof(n.source));
    memcpy(n.amount, v->amount, sizeof(n.amount));
    n.override = v->override;
    if (memcmp(&n, v, sizeof(n))) {
        filt_held = *v;
        filt_held_owner = fx_owner();
        filt_apply(&n);
        ui_say("FILTER CLEARED");
    } else if (filt_held_owner == fx_owner()) {
        n = filt_held;
        memcpy(n.source, v->source, sizeof(n.source));
        memcpy(n.amount, v->amount, sizeof(n.amount));
        filt_held_owner = 0;
        filt_apply(&n);
        ui_say("FILTER RESTORED");
    }
}

static const struct field f_ftype = { get_fval, set_fval, range_fval, fmt_fval, 0, L_VLIST, ftype_labels, hold_ftype };
static const struct field f_fval = { get_fval, set_fval, range_fval, fmt_fval, 0, L_NONE, 0, 0 };

static int (*mx_coarse(const struct field *f))(int, int, int);
static int (*gr_coarse(const struct field *f))(int, int, int);
static int (*tl_coarse(const struct field *f))(int, int, int);

/* DATA1 held while turning in an edit: the large step of f, if it has one. */
static int (*fx_coarse(const struct field *f))(int, int, int)
{
    return f == &f_eqf ? coarse_freq : f == &f_eqg || f == &f_eqamt || f == &f_famt ? coarse_ten :
           f == &f_eqq ? coarse_q : f == &f_eqhpf ? coarse_hpf : f == &f_ftype || f == &f_fval ? coarse_fval :
           mx_coarse(f) ? mx_coarse(f) : gr_coarse(f) ? gr_coarse(f) : tl_coarse(f);
}

/* Response graphs: 20 Hz..20 kHz, log; 100 Hz, 1 kHz, 10 kHz and 0 dB
 * dotted. Curves are worked out (double maths, a few ms) only when the
 * settings or their CV change. */
#define GR_X 12
#define GR_W 116
#define GR_PTS (GR_W - 2)

static void graph_frame(int y, int h, int mid)
{
    gfx_frame(GR_X, y, GR_W, h, C_FAINT);
    for (int x = GR_X + 1; x < GR_X + GR_W - 1; x += 2) {
        gfx_rect(x, mid, 1, 1, C_FAINT);
    }
    for (int d = 0; d < 3; d++) {
        int x = GR_X + 1 + (int)(eq_frequency_position(d == 0 ? 100.0f : d == 1 ? 1000.0f : 10000.0f) * (GR_PTS - 1) + 0.5f);
        for (int yy = y + 1; yy < y + h - 1; yy += 2) {
            gfx_rect(x, yy, 1, 1, C_FAINT);
        }
    }
}

static void graph_curve(const uint8_t *c, uint16_t color)
{
    for (int n = 0; n < GR_PTS; n++) {
        int a = c[n ? n - 1 : 0], b = c[n];
        if (a > b) {
            int t = a;
            a = b;
            b = t;
        }
        gfx_rect(GR_X + 1 + n, a, 1, b - a + 1, color);
    }
}

static int graph_x(float pos)
{
    return GR_X + 1 + (int)(pos * (GR_PTS - 1) + 0.5f);
}

#define EQ_GR_Y 13
#define EQ_GR_H 31

static void eq_graph(void)
{
    static struct eq_value drawn;
    static uint8_t curve[GR_PTS];
    static int valid;
    static uint32_t at;
    const struct eq_value *v = eq_cur();
    struct eq_value live;
    int top = EQ_GR_Y + 1, bot = EQ_GR_Y + EQ_GR_H - 2, f = focus[page];

    if (eq_modulated(v) && valid && time_us() - at < 40000u) {
        live = drawn;
    } else {
        eq_modulate(v, (u32)fx_ch(), &live);
        at = time_us();
    }
    live.on = 1;
    live.override = 0;
    if (!valid || memcmp(&live, &drawn, sizeof(live))) {
        eq_curve(&live, GR_PTS, curve, top, bot);
        drawn = live;
        valid = 1;
    }
    graph_frame(EQ_GR_Y, EQ_GR_H, (top + bot) / 2);
    graph_curve(curve, v->on && !eq_following() ? C_WHITE : C_DIM);
    /* Band handles; the band being edited (or its CV route) yellow. */
    int band = page == P_EQ ? (f < 9 ? f % 3 : -1) : (f / 2 < EQ_MOD_HPF ? f / 4 : -1);
    for (int b = 0; b < 3; b++) {
        int x = graph_x(eq_step_position(live.frequency[b])), y = curve[x - GR_X - 1] - 1;
        x = x - 1 < GR_X + 1 ? GR_X + 1 : x - 1 > GR_X + GR_W - 4 ? GR_X + GR_W - 4 : x - 1;
        y = y < top ? top : y > bot - 2 ? bot - 2 : y;
        gfx_rect(x, y, 3, 3, b == band ? C_YELLOW : C_TITLE);
    }
}

static void fx_hint(const char *s)
{
    gfx_text(12, 118, s, C_FAINT);
}

static void draw_eq(void)
{
    static const char *const bands[3] = { "LOW", "MID", "HIGH" };
    int f;

    spage("EQ", fx_zoned());
    eq_graph();
    field_dim = eq_following();
    for (int b = 0; b < 3; b++) {
        gfx_title(12 + 40 * b, 46, bands[b], C_TITLE);
    }
    for (int b = 0; b < 3; b++) {
        field(12 + 40 * b, 56, &f_eqf, b, 0);
    }
    for (int b = 0; b < 3; b++) {
        field(12 + 40 * b, 66, &f_eqg, b, 0);
    }
    for (int b = 0; b < 3; b++) {
        field(12 + 40 * b, 76, &f_eqq, b, 0);
    }
    gfx_title(12, 90, "HPF", C_TITLE);
    gfx_title(52, 90, "EQ", C_TITLE);
    field(12, 100, &f_eqhpf, 0, 0);
    field(52, 100, &f_eqon, 0, 0);
    field_dim = 0;
    f = focus[P_EQ];
    if (is_sampler(fx_ch())) {
        int hold = f == 11 && fx_zoned() && fx_zone()->eq.override;
        gfx_title(hold ? 72 : 92, 90, hold ? "HOLD:CLR" : "EDIT", C_TITLE);
        field(92, 100, &f_eqscope, 0, 0);
    }
    if (f == 10 && !eq_following()) {
        struct eq_value n;
        eq_flat(&n);
        if (memcmp(&n, eq_cur(), sizeof(n))) {
            fx_hint("HOLD: CLEAR");
        } else if (eq_held_owner == fx_owner()) {
            fx_hint("HOLD: RESTORE");
        }
    } else if (f < 3) {
        char s[24];
        unsigned c = eq_step_centihertz((u32)eq_cur()->frequency[f]);
        fmt_fixed(s, (int)c, 2, 1, 0);
        cat(s, " HZ");
        fx_hint(s);
    }
}

/* What a volt does at the focused amount, e.g. "+0.50 OCT/V"; x has dec
 * decimals. */
static void per_volt_hint(int x, int dec, const char *unit)
{
    char s[24];

    fmt_fixed(s, x, dec, 1, 1);
    cat(s, " ");
    cat(s, unit);
    cat(s, "/V");
    fx_hint(s);
}

static void draw_eqcv(void)
{
    static const char *const names[EQ_MOD_COUNT] = { "LO FRQ", "LO GN", "MD FRQ", "MD GN", "HI FRQ", "HI GN", "HPF" };
    int f = focus[P_EQCV];

    spage("EQ CV", fx_zoned());
    field_dim = eq_following();
    for (int m = 0; m < EQ_MOD_COUNT; m++) {
        int y = 16 + m * 13;
        gfx_text(12, y, names[m], C_TITLE);
        field(52, y, &f_eqsrc, m, 0);
        field(80, y, &f_eqamt, m, 0);
        gfx_bar_bipolar(113, y + 1, 14, 5, (float)eq_cur()->amount[m] / 100.0f, C_GREEN);
    }
    field_dim = 0;
    if (f / 2 < EQ_MOD_COUNT && eq_cur()->source[f / 2] != TA_SRC_OFF) {
        int m = f / 2, a = eq_cur()->amount[m];
        if (m % 2 && m != EQ_MOD_HPF) {
            per_volt_hint(a * EQ_CV_GAIN_TENTHS_PER_VOLT / 100, 1, "DB");
        } else {
            per_volt_hint(a, 2, "OCT");
        }
    }
}

#define F_GR_Y 13
#define F_GR_H 23
#define F_DB_TOP 18
#define F_DB_BOT (-42)

static void filt_graph(void)
{
    static struct filter_value drawn[2];
    static uint8_t curve[2][GR_PTS];
    static int valid[2];
    const struct filter_value *v = filt_cur();
    struct filter_value live, side[2];
    int top = F_GR_Y + 1, bot = F_GR_Y + F_GR_H - 2, mid = top + (bot - top) * F_DB_TOP / (F_DB_TOP - F_DB_BOT);
    int gray = v->type == FILTER_OFF || filt_following(), both = filt_paired() && v->right;

    filter_live(v, (u32)fx_ch(), &live);
    side[0] = side[1] = live;
    side[1].cutoff = (uint16_t)filter_right_cutoff(&live);
    graph_frame(F_GR_Y, F_GR_H, mid);
    for (int k = 0; k < 2; k++) {
        side[k].right = 0;
        side[k].override = 0;
        if (!valid[k] || memcmp(&side[k], &drawn[k], sizeof(side[k]))) {
            if (side[k].type == FILTER_OFF) {
                memset(curve[k], mid, GR_PTS);
            } else {
                filter_curve(&side[k], GR_PTS, curve[k], top, bot, F_DB_TOP, F_DB_BOT);
            }
            drawn[k] = side[k];
            valid[k] = 1;
        }
    }
    int ed = filt_right();
    if (both) {
        graph_curve(curve[!ed], gray ? C_FAINT : C_TITLE);
    }
    graph_curve(curve[ed], gray ? C_DIM : C_WHITE);
}

static void draw_filter(void)
{
    const struct filter_value *v = filt_cur();
    int extra = v->type == FILTER_BAND_STOP || v->type == FILTER_DUAL, f;
    const char *cut = filt_right() ? "CUTOFF R" : filt_paired() ? "CUTOFF L" : "CUTOFF";
    struct field_ref fr;

    spage("FILTER", fx_zoned());
    filt_graph();
    field_dim = filt_following();
    gfx_title(12, 39, "TYPE", C_TITLE);
    gfx_title(70, 39, cut, C_TITLE);
    field(12, 49, &f_ftype, FILTER_MOD_TYPE, 0);
    field(70, 49, &f_fval, FILTER_MOD_CUTOFF, 0);
    field(12, 59, &f_fsrc, FILTER_MOD_TYPE, 0);
    field(34, 59, &f_famt, FILTER_MOD_TYPE, 0);
    field(70, 59, &f_fsrc, FILTER_MOD_CUTOFF, 0);
    field(92, 59, &f_famt, FILTER_MOD_CUTOFF, 0);
    gfx_title(12, 73, "RES", C_TITLE);
    field(12, 83, &f_fval, FILTER_MOD_RESONANCE, 0);
    if (extra) {
        gfx_title(70, 73, v->type == FILTER_DUAL ? "OFFSET" : "SPREAD", C_TITLE);
        field(70, 83, &f_fval, FILTER_MOD_EXTRA, 0);
    }
    field(12, 93, &f_fsrc, FILTER_MOD_RESONANCE, 0);
    field(34, 93, &f_famt, FILTER_MOD_RESONANCE, 0);
    if (extra) {
        field(70, 93, &f_fsrc, FILTER_MOD_EXTRA, 0);
        field(92, 93, &f_famt, FILTER_MOD_EXTRA, 0);
    }
    field_dim = 0;
    if (is_sampler(fx_ch())) {
        field(76, 2, &f_fscope, 0, 0);
    }
    f = focus[P_FILTER];
    if (f >= nfields) {
        return;
    }
    fr = fields[f];
    if (fr.f == &f_ftype && !filt_following()) {
        struct filter_value n;
        filter_value_defaults(&n);
        memcpy(n.source, v->source, sizeof(n.source));
        memcpy(n.amount, v->amount, sizeof(n.amount));
        n.override = v->override;
        if (memcmp(&n, v, sizeof(n))) {
            fx_hint("HOLD: CLEAR");
        } else if (filt_held_owner == fx_owner()) {
            fx_hint("HOLD: RESTORE");
        }
    } else if (fr.f == &f_fscope && fx_zoned() && fx_zone()->filter.override) {
        fx_hint("HOLD: FOLLOW CHANNEL");
    } else if (fr.f == &f_famt && v->source[fr.arg] != TA_SRC_OFF) {
        int a = v->amount[fr.arg];
        if (fr.arg == FILTER_MOD_CUTOFF) {
            per_volt_hint(a * FILTER_CUTOFF_OCTAVES_PER_VOLT, 2, "OCT");
        } else if (fr.arg == FILTER_MOD_TYPE) {
            per_volt_hint(a * FILTER_TYPE_STEPS_PER_VOLT, 2, "TYPES");
        } else {
            per_volt_hint(a * FILTER_STEPS_PER_VOLT, 2, "STEPS");
        }
    }
}

/* ---- Comp, Sends, Reverb, Delay, Mix Comp (Tallent Edition pages, our
 * look). PAN/MIX -> COMP -> SENDS -> PAN/MIX; CHANNELS -> REVERB -> DELAY ->
 * MIX COMP -> CHANNELS.
 * Comp and Sends are the selected channel's (a Stereo/Right pair's master);
 * the rest are the mix's. The settings are preset.mixfx (all defaults until
 * the first edit, as Tallent). */

enum { MK_RATIO, MK_THRESH, MK_ATTACK, MK_RELEASE, MK_PERCENT, MK_GAIN, MK_KEY, MK_FEEDBACK, MK_TAP, MK_DECAY,
       MK_DIVISION, MK_LOWCUT, MK_HIGHCUT, MK_ALGO };

struct mx_def {
    uint8_t kind, lo, hi, stride;
    uint8_t off;
    int8_t route;           /* send or bus route, -1 none */
};

#define MXC(f) offsetof(struct mix_settings, comp[0].f), -1
#define MXM(f) offsetof(struct mix_settings, master.f), -1
#define MXB(f) offsetof(struct mix_settings, f)

/* arg of the mix fields: an index here. */
enum { MX_COMP = 0, MX_MASTER = 8, MX_REVSEND = 16, MX_DLYSEND, MX_PRE, MX_ALGO, MX_DECAY, MX_DAMP, MX_RLEVEL,
       MX_RATE, MX_DEPTH, MX_TIME, MX_FEEDBACK, MX_LOWCUT, MX_HIGHCUT, MX_DLEVEL, MX_COUNT };
static const struct mx_def mx_defs[MX_COUNT] = {
    { MK_RATIO, 0, MIX_RATIOS - 1, 8, MXC(ratio) },
    { MK_THRESH, 0, MIX_THRESHOLD_MAX, 8, MXC(threshold) },
    { MK_ATTACK, 0, MIX_ATTACKS - 1, 8, MXC(attack) },
    { MK_RELEASE, 0, MIX_RELEASES - 1, 8, MXC(release) },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 8, MXC(drive) },
    { MK_GAIN, 0, MIX_GAIN_MAX, 8, MXC(gain) },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 8, MXC(mix) },
    { MK_KEY, 0, MIX_KEY_INPUT, 8, MXC(key) },
    { MK_RATIO, 0, MIX_RATIOS - 1, 0, MXM(ratio) },
    { MK_THRESH, 0, MIX_THRESHOLD_MAX, 0, MXM(threshold) },
    { MK_ATTACK, 0, MIX_ATTACKS - 1, 0, MXM(attack) },
    { MK_RELEASE, 0, MIX_RELEASES - 1, 0, MXM(release) },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXM(drive) },
    { MK_GAIN, 0, MIX_GAIN_MAX, 0, MXM(gain) },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXM(mix) },
    { MK_KEY, 0, MIX_KEY_INPUT, 0, MXM(key) },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 1, MXB(reverb_send), MIX_ROUTE_REVERB_SEND },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 1, MXB(delay_send), MIX_ROUTE_DELAY_SEND },
    { MK_TAP, 0, 1, 1, MXB(send_pre), -1 },
    { MK_ALGO, 0, MIX_ALGOS - 1, 0, MXB(reverb_algo), -1 },
    { MK_DECAY, MIX_DECAY_MIN, MIX_DECAY_MAX, 0, MXB(reverb_decay), MIX_ROUTE_REVERB_DECAY },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXB(reverb_damp), MIX_ROUTE_REVERB_DAMP },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXB(reverb_level), MIX_ROUTE_REVERB_LEVEL },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXB(chorus_rate), MIX_ROUTE_CHORUS_RATE },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXB(chorus_depth), MIX_ROUTE_CHORUS_DEPTH },
    { MK_DIVISION, 0, MIX_DIVISIONS - 1, 0, MXB(delay_division), MIX_ROUTE_DELAY_TIME },
    { MK_FEEDBACK, 0, MIX_FEEDBACK_MAX, 0, MXB(delay_feedback), MIX_ROUTE_DELAY_FEEDBACK },
    { MK_LOWCUT, 0, MIX_FILTER_STEPS - 1, 0, MXB(delay_low), MIX_ROUTE_DELAY_LOW },
    { MK_HIGHCUT, 0, MIX_FILTER_STEPS - 1, 0, MXB(delay_high), MIX_ROUTE_DELAY_HIGH },
    { MK_PERCENT, 0, MIX_PERCENT_MAX, 0, MXB(delay_level), MIX_ROUTE_DELAY_LEVEL },
};

static const char *const mx_ratio_labels[MIX_RATIOS] = { "OFF", "1.5:1", "2:1", "3:1", "4:1", "6:1", "10:1", "20:1",
                                                         "LIMIT" };
static const char *const mx_key_labels[MIX_KEY_INPUT + 1] = { "SELF", "CH 1", "CH 2", "CH 3", "CH 4", "CH 5", "CH 6",
                                                              "CH 7", "CH 8", "INPUT" };
static const char *const mx_tap_labels[2] = { "POST", "PRE" };
static const char *const mx_algo_labels[MIX_ALGOS] = { "1 CHORUS", "2 REVERB", "3 REV MOD", "4 CHO REV",
                                                       "5 REV+CHO", "6 SER/PAR", "7 NONLIN", "8 NL MOD",
                                                       "9 NL CHORUS", "11 LD NL MOD" };
static const char *const mx_division_labels[MIX_DIVISIONS] = { "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4",
                                                               "1/2T", "1/4D", "1/2", "1/1" };
static const char *const mx_lowcut_labels[MIX_FILTER_STEPS] = { "OFF", "20HZ", "50HZ", "100HZ", "200HZ", "400HZ",
                                                                "800HZ" };
static const char *const mx_highcut_labels[MIX_FILTER_STEPS] = { "1K", "2K", "3K", "5K", "8K", "12K", "OFF" };

static int mx_ch(int a) { return mx_defs[a].stride ? fx_ch() : 0; }
static unsigned mx_at(int a) { return mx_defs[a].off + (unsigned)(mx_ch(a) * mx_defs[a].stride); }
static int get_mx(int a) { return ((const uint8_t *)mix_now())[mx_at(a)]; }
static void set_mx(int a, int v)
{
    ((uint8_t *)mix_edit())[mx_at(a)] = (uint8_t)v;
}
static void range_mx(int a, int *lo, int *hi)
{
    *lo = mx_defs[a].lo;
    *hi = mx_defs[a].hi;
}

static int mx_gated(void) { return !preset.reverb_unit && mix_now()->reverb_algo >= MIX_ALGO_GATED; }

static void fmt_mx(char *o, int v, int a)
{
    o[0] = 0;
    switch (mx_defs[a].kind) {
    case MK_RATIO: cat(o, mx_ratio_labels[v]); break;
    case MK_THRESH: fmt_int(o, -v); cat(o, " DB"); break;
    case MK_ATTACK: {
        int t = mix_attack_hundredths_ms[v];
        if (t < 100) {
            fmt_fixed(o, t, 2, 1, 0);
        } else {
            fmt_int(o, t / 100);
        }
        cat(o, " MS");
        break;
    }
    case MK_RELEASE:
        if (mix_release_ms[v] < 1000) {
            fmt_int(o, mix_release_ms[v]);
            cat(o, " MS");
        } else {
            fmt_fixed(o, mix_release_ms[v] / 100, 1, 1, 0);
            cat(o, " S");
        }
        break;
    case MK_GAIN: fmt_int(o, v); cat(o, " DB"); if (v) { char t[12] = "+"; cat(t, o); memcpy(o, t, 12); } break;
    case MK_KEY: cat(o, mx_key_labels[v]); break;
    case MK_TAP: cat(o, mx_tap_labels[v]); break;
    case MK_ALGO: cat(o, mx_algo_labels[v]); break;
    case MK_DIVISION: cat(o, mx_division_labels[v]); break;
    case MK_LOWCUT: cat(o, mx_lowcut_labels[v]); break;
    case MK_HIGHCUT: cat(o, mx_highcut_labels[v]); break;
    case MK_DECAY:
        if (mx_gated()) {
            fmt_int(o, v * 5);
            cat(o, " MS");
        } else {
            fmt_fixed(o, v, 1, 1, 0);
            cat(o, " S");
        }
        break;
    default: fmt_int(o, v); cat(o, "%"); break;
    }
}

static const struct field f_mx = { get_mx, set_mx, range_mx, fmt_mx, 0, L_NONE, 0, 0 };
static const struct field f_mxratio = { get_mx, set_mx, range_mx, fmt_mx, 0, L_VLIST, mx_ratio_labels, 0 };
static const struct field f_mxkey = { get_mx, set_mx, range_mx, fmt_mx, 0, L_VLIST, mx_key_labels, 0 };
static const struct field f_mxtap = { get_mx, set_mx, range_mx, fmt_mx, 0, L_VLIST, mx_tap_labels, 0 };
/* The Reverb page's UNIT, ALGO and PROGRAM rows, each a one-level list:
 * the units, the unit's algorithms, the algorithm's factory programs
 * (units with programs). A new unit is a row of rv_units and a case in
 * the rvp_ / rvf_ functions below. */
static const char *const rv_80_algos[R80_ALGOS] = { "CHAMBER", "PLATE", "CONCERT HALL", "INVERSE", "CHORUS+RVB" };
static const struct rv_unit {
    const char *name;
    uint8_t id;             /* preset.reverb_unit */
    uint8_t algos, programs;
    const char *const *algo_names;
} rv_units[] = {
    { "DEP-5", 0, MIX_ALGOS, 0, mx_algo_labels },
    { "70", REVERB_70, R70_ALGOS, 1, r70_algo_names },
    { "80", REVERB_80, R80_ALGOS, 1, rv_80_algos },
};
#define RV_UNITS ((int)(sizeof(rv_units) / sizeof(rv_units[0])))

static struct {
    uint8_t mixfx[TA_MIXFX_BYTES];
    uint8_t unit, program;
    int16_t edit[REVERB_EDITS];
} rv_saved;

static int rv_unit_now(void)
{
    for (int u = 0; u < RV_UNITS; u++) {
        if (rv_units[u].id == preset.reverb_unit) {
            return u;
        }
    }
    return 0;
}
static int rv_has_programs(void) { return rv_units[rv_unit_now()].programs; }
static void rv_clear_edits(void)
{
    for (int e = 0; e < REVERB_EDITS; e++) {
        preset.reverb_edit[e] = -1;
    }
}

/* The unit's programs, in its own order. */
static unsigned rvp_count(int id)
{
    return id == REVERB_70 ? r70_program_count : id == REVERB_80 ? r80_program_count : 0;
}
static int rvp_algo(int id, unsigned i) { return id == REVERB_70 ? r70_programs[i].algo : r80_programs[i].algo; }
static unsigned rvp_number(int id, unsigned i)
{
    return id == REVERB_70 ? r70_programs[i].number : r80_programs[i].number;
}
static const char *rvp_name(int id, unsigned i) { return id == REVERB_70 ? r70_programs[i].name : r80_programs[i].name; }
static int rvp_cur(void)
{
    for (unsigned i = 0; i < rvp_count(preset.reverb_unit); i++) {
        if (rvp_number(preset.reverb_unit, i) == preset.reverb_program) {
            return (int)i;
        }
    }
    return -1;
}
/* The k-th program of algorithm algo, -1 none; k < 0: how many it has. */
static int rvp_nth(int id, int algo, int k)
{
    int n = 0;

    for (unsigned i = 0; i < rvp_count(id); i++) {
        if (rvp_algo(id, i) == algo) {
            if (n == k) {
                return (int)i;
            }
            n++;
        }
    }
    return k < 0 ? n : -1;
}
static void rv_pick(int id, int algo)
{
    int i = rvp_nth(id, algo, 0);

    preset.reverb_unit = (uint8_t)id;
    preset.reverb_program = (uint8_t)(i >= 0 ? rvp_number(id, (unsigned)i) : 0);
}

/* UNIT: a new unit starts on its first algorithm and program. */
static int get_rvunit(int a) { (void)a; return rv_unit_now(); }
static void set_rvunit(int a, int v)
{
    rv_clear_edits();
    if (!rv_units[v].programs) {
        preset.reverb_unit = 0;
        preset.reverb_program = 0;
        set_mx(MX_ALGO, 0);
    } else {
        rv_pick(rv_units[v].id, 0);
    }
    (void)a;
}
static void range_rvunit(int a, int *lo, int *hi)
{
    (void)a;
    *lo = 0;
    *hi = RV_UNITS - 1;
}
static void fmt_rvunit(char *o, int v, int a)
{
    (void)a;
    o[0] = 0;
    cat(o, rv_units[v].name);
}
static const struct field f_rvunit = { get_rvunit, set_rvunit, range_rvunit, fmt_rvunit, 0, L_REVERB, 0, 0 };

/* ALGO: the unit's algorithm; a new one starts on its first program. */
static int get_rvalgo(int a)
{
    int c = rvp_cur();

    if (rv_has_programs()) {
        return c >= 0 ? rvp_algo(preset.reverb_unit, (unsigned)c) : 0;
    }
    return get_mx(a);
}
static void set_rvalgo(int a, int v)
{
    rv_clear_edits();
    if (rv_has_programs()) {
        rv_pick(preset.reverb_unit, v);
    } else {
        set_mx(a, v);
    }
}
static void range_rvalgo(int a, int *lo, int *hi)
{
    (void)a;
    *lo = 0;
    *hi = rv_units[rv_unit_now()].algos - 1;
}
static void fmt_rvalgo(char *o, int v, int a)
{
    (void)a;
    o[0] = 0;
    cat(o, rv_units[rv_unit_now()].algo_names[v]);
}
static const struct field f_mxalgo = { get_rvalgo, set_rvalgo, range_rvalgo, fmt_rvalgo, 0, L_REVERB, 0, 0 };

/* PROGRAM: the position in the algorithm's programs; picking clears the
 * edits. */
static int get_rvprog(int a)
{
    int c = rvp_cur(), n = 0;

    (void)a;
    for (int i = 0; i < c; i++) {
        n += rvp_algo(preset.reverb_unit, (unsigned)i) == rvp_algo(preset.reverb_unit, (unsigned)c);
    }
    return c >= 0 ? n : 0;
}
static void set_rvprog(int a, int v)
{
    int i = rv_has_programs() ? rvp_nth(preset.reverb_unit, get_rvalgo(MX_ALGO), v) : -1;

    (void)a;
    if (i >= 0) {
        rv_clear_edits();
        preset.reverb_program = (uint8_t)rvp_number(preset.reverb_unit, (unsigned)i);
    }
}
static void range_rvprog(int a, int *lo, int *hi)
{
    int n = rv_has_programs() ? rvp_nth(preset.reverb_unit, get_rvalgo(MX_ALGO), -1) : 0;

    (void)a;
    *lo = 0;
    *hi = n > 0 ? n - 1 : 0;
}
static void fmt_rvprog(char *o, int v, int a)
{
    int i = rv_has_programs() ? rvp_nth(preset.reverb_unit, get_rvalgo(MX_ALGO), v) : -1;

    (void)a;
    o[0] = 0;
    cat(o, i >= 0 ? rvp_name(preset.reverb_unit, (unsigned)i) : "---");
}
static const struct field f_rvprog = { get_rvprog, set_rvprog, range_rvprog, fmt_rvprog, 0, L_RVPROG, 0, 0 };

/* s holds at least 64 bytes: a list row (a program with its number). */
static void rv_label(char *s, int v)
{
    s[0] = 0;
    if (pop.ref.f == &f_rvprog) {
        int i = rvp_nth(preset.reverb_unit, get_rvalgo(MX_ALGO), v);
        if (i >= 0) {
            cat(put_uint(s, rvp_number(preset.reverb_unit, (unsigned)i), 3), " ");
            cat(s, rvp_name(preset.reverb_unit, (unsigned)i));
        }
    } else {
        pop.ref.f->fmt(s, v, pop.ref.arg);
    }
}

/* The pickers' cancel puts back the unit, program, edits and the DEP-5's
 * settings. */
static void rv_save(void)
{
    memcpy(rv_saved.mixfx, preset.mixfx, sizeof(rv_saved.mixfx));
    rv_saved.unit = preset.reverb_unit;
    rv_saved.program = preset.reverb_program;
    memcpy(rv_saved.edit, preset.reverb_edit, sizeof(rv_saved.edit));
}
static void rv_restore(void)
{
    __asm__ volatile("cpsid i" ::: "memory");
    memcpy(preset.mixfx, rv_saved.mixfx, sizeof(rv_saved.mixfx));
    preset.reverb_unit = rv_saved.unit;
    preset.reverb_program = rv_saved.program;
    memcpy(preset.reverb_edit, rv_saved.edit, sizeof(rv_saved.edit));
    __asm__ volatile("cpsie i" ::: "memory");
}

static const struct field f_mxdiv = { get_mx, set_mx, range_mx, fmt_mx, 0, L_VLIST, mx_division_labels, 0 };

/* CV routes of the sends (a channel's: CV A-C allowed) and of the bus
 * (inputs 1A-8C only, as Tallent: L_D2CV's list, 0 = Off). arg = mx_defs
 * index. */
static struct mix_route *mx_route(int a, int edit)
{
    struct mix_settings *m = edit ? mix_edit() : (struct mix_settings *)mix_now();

    return mx_defs[a].stride ? &m->send_route[fx_ch()][mx_defs[a].route] : &m->bus_route[mx_defs[a].route];
}
static int get_mxsrc(int a)
{
    int s = mx_route(a, 0)->source;

    return mx_defs[a].stride ? s - TA_SRC_OFF : s == TA_SRC_OFF ? 0 : s + 1;
}
static void set_mxsrc(int a, int v)
{
    mx_route(a, 1)->source = (signed char)(mx_defs[a].stride ? v + TA_SRC_OFF : v ? v - 1 : TA_SRC_OFF);
}
static int get_mxamt(int a) { return mx_route(a, 0)->amount; }
static void set_mxamt(int a, int v) { mx_route(a, 1)->amount = (signed char)v; }
static const struct field f_mxsrc = { get_mxsrc, set_mxsrc, range_src, fmt_src_short, 0, L_CVSRC, 0, 0 };
static const struct field f_mxbsrc = { get_mxsrc, set_mxsrc, range_d2cv, fmt_d2cv, 0, L_D2CV, 0, 0 };
static const struct field f_mxamt = { get_mxamt, set_mxamt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

/* The program's fields (the 70's or the 80's edit slots); arg = the slot.
 * Shown: the edit, else the value playing now, else the program's. Hold
 * DATA1 goes back to the program's. */
static const struct r70_program *rv_p70(void) { return preset.reverb_unit == REVERB_70 ? r70_find(preset.reverb_program) : 0; }
static const struct r80_program *rv_p80(void) { return preset.reverb_unit == REVERB_80 ? r80_find(preset.reverb_program) : 0; }
static int rv_prog_ok(void) { return rv_p70() || rv_p80(); }
static unsigned rvf_fields(uint8_t *edit)
{
    return rv_p70() ? r70_fields(rv_p70(), edit) : rv_p80() ? r80_fields(rv_p80(), edit) : 0;
}
static const char *rvf_name(unsigned e)
{
    static char s[16];

    return rv_p70() ? r70_edit_name(rv_p70(), e, s) : r80_edit_name(rv_p80(), e);
}
/* The value playing now, -1 none. */
static int rvf_live(unsigned e)
{
    if (rv_p70()) {
        return r70_running() == rv_p70() ? r70_live(e) : -1;
    }
    return rv_p80() && r80_running() == rv_p80() ? r80_live(e) : -1;
}
static int rvf_base(unsigned e)
{
    return rv_p70() ? r70_edit_base(rv_p70(), e) : rv_p80() ? r80_edit_base(rv_p80(), e) : 0;
}
static int get_rv(int e)
{
    int v = preset.reverb_edit[e];

    if (v < 0) {
        v = rvf_live((unsigned)e);
    }
    return v >= 0 ? v : rvf_base((unsigned)e);
}
static void set_rv(int e, int v) { preset.reverb_edit[e] = (int16_t)v; }
static void range_rv(int e, int *lo, int *hi)
{
    *lo = *hi = 0;
    if (rv_p70()) {
        r70_edit_range(rv_p70(), (unsigned)e, lo, hi);
    } else if (rv_p80()) {
        r80_edit_range(rv_p80(), (unsigned)e, lo, hi);
    }
}
static void fmt_rv(char *o, int v, int e)
{
    o[0] = 0;
    if (rv_p70()) {
        r70_edit_format(rv_p70(), (unsigned)e, v, o);
    } else if (rv_p80()) {
        r80_edit_format(rv_p80(), (unsigned)e, v, o);
    }
}
static void hold_rv(int e)
{
    preset.reverb_edit[e] = -1;
    ui_say("PROGRAM VALUE");
}
static int coarse_rv(int e, int v, int s)
{
    int lo, hi;

    range_rv(e, &lo, &hi);
    return hi - lo >= 40 ? coarse_step(v, s, 10, 0) : v + s;
}
static const struct field f_rv = { get_rv, set_rv, range_rv, fmt_rv, 0, L_NONE, 0, hold_rv };

static int get_rvsrc(int e) { int s = preset.reverb_route[e].source; return s < 0 ? 0 : s + 1; }
static void set_rvsrc(int e, int v) { preset.reverb_route[e].source = (signed char)(v ? v - 1 : TA_SRC_OFF); }
static int get_rvamt(int e) { return preset.reverb_route[e].amount; }
static void set_rvamt(int e, int v) { preset.reverb_route[e].amount = (signed char)v; }
static const struct field f_rvsrc = { get_rvsrc, set_rvsrc, range_d2cv, fmt_d2cv, 0, L_D2CV, 0, 0 };
static const struct field f_rvamt = { get_rvamt, set_rvamt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

static int coarse_mx(int a, int v, int s)
{
    int k = mx_defs[a].kind;

    return k == MK_PERCENT || k == MK_FEEDBACK || k == MK_DECAY || k == MK_THRESH
               ? coarse_step(v, s, k == MK_THRESH ? 6 : 10, 0)
               : v + s;
}

static const struct field *mx_value_field(int a)
{
    switch (mx_defs[a].kind) {
    case MK_RATIO: return &f_mxratio;
    case MK_KEY: return &f_mxkey;
    case MK_TAP: return &f_mxtap;
    case MK_ALGO: return &f_mxalgo;
    case MK_DIVISION: return &f_mxdiv;
    default: return &f_mx;
    }
}

/* A setting: its title, value and (if routed) source and amount under it. */
static void mx_block(int x, int y, const char *title, int a, int dim)
{
    gfx_title(x, y, title, dim ? C_DIM : C_TITLE);
    field_dim = dim;
    field(x, y + 10, mx_value_field(a), a, 0);
    if (mx_defs[a].route >= 0) {
        field(x, y + 20, mx_defs[a].stride ? &f_mxsrc : &f_mxbsrc, a, 0);
        field(x + 22, y + 20, &f_mxamt, a, 0);
    }
    field_dim = 0;
}

static int mx_paired(void) { int m = fx_ch(); return m + 1 < CHANNELS && preset.ch[m + 1].mode == MODE_STEREO; }

/* "1+2" for a stereo pair (both share the master's compressor and sends). */
static void mx_pair_note(void)
{
    if (mx_paired()) {
        char s[4] = { (char)('1' + fx_ch()), '+', (char)('2' + fx_ch()), 0 };
        gfx_text_right(127, 2, s, C_YELLOW);
    }
}

/* Gain reduction: a segment for each 2 dB up to 24 dB, and the dB. */
static void mx_meter(int y, int comp, int on)
{
    unsigned d = on ? fx_reduction_db(comp) : 0;
    char s[12];

    for (int i = 0; i < 12; i++) {
        gfx_rect(12 + i * 6, y, 5, 5, on && d >= (unsigned)(i + 1) * 2 ? C_YELLOW : C_FAINT);
    }
    if (on) {
        s[0] = 0;
        fmt_int(s, -(int)d);
        cat(s, " DB");
        gfx_text_right(127, y - 1, s, C_WHITE);
    }
}

static void draw_comp_page(const char *title, int base, int comp)
{
    int on = get_mx(base) != 0;

    if (base == MX_COMP) {
        spage(title, 0);
        mx_pair_note();
    } else {
        draw_header(title);
    }
    gfx_title(12, 14, "GR", C_TITLE);
    mx_meter(24, comp, on);
    mx_block(12, 32, "RATIO", base, 0);
    mx_block(70, 32, "THRESH", base + 1, !on);
    mx_block(12, 54, "ATTACK", base + 2, !on);
    mx_block(70, 54, "RELEASE", base + 3, !on);
    mx_block(12, 76, "DRIVE", base + 4, 0);
    mx_block(70, 76, "GAIN", base + 5, !on);
    mx_block(12, 98, "MIX", base + 6, 0);
    mx_block(70, 98, "SIDECH", base + 7, !on);
}

static void draw_comp(void) { draw_comp_page("COMP", MX_COMP, fx_ch()); }
static void draw_mixcomp(void) { draw_comp_page("MIX COMP", MX_MASTER, CHANNELS); }

/* What a volt does at the focused amount: at +1.00 5 V crosses the range. */
static void mx_hint(void)
{
    int f = focus[page];

    if (f >= nfields) {
        return;
    }
    struct field_ref r = fields[f];
    int a = r.arg;
    if (r.f == &f_mxamt && get_mxsrc(a)) {
        const struct mx_def *d = &mx_defs[a];
        if (d->kind == MK_PERCENT || d->kind == MK_FEEDBACK) {
            per_volt_hint(get_mxamt(a) * (d->hi - d->lo) / 50, 1, "%");
        } else if (d->kind == MK_DECAY) {
            per_volt_hint(get_mxamt(a) * (d->hi - d->lo) / 500, 2, "S");
        } else {
            per_volt_hint(get_mxamt(a) * (d->hi - d->lo) / 50, 1, "STEPS");
        }
    } else if (r.f == &f_mxtap) {
        fx_hint(get_mx(a) ? "BEFORE MIX LEVEL, MUTE" : "AFTER MIX LEVEL, MUTE");
    } else if (r.f == &f_mx && (a == MX_RATE || a == MX_DEPTH)) {
        char s[24];
        int v = get_mx(a);
        if (a == MX_RATE) {
            /* 0.08 Hz at 0 to 5 Hz at 100, exponential (mix_fx.c). */
            fmt_fixed(s, (int)(8.0f * fast_exp2(5.97f * (float)v / 100.0f) + 0.5f), 2, 1, 0);
            cat(s, " HZ");
        } else {
            fmt_fixed(s, v * 5, 2, 1, 0);
            cat(s, " MS SWEEP");
        }
        fx_hint(s);
    }
}

static void draw_sends(void)
{
    spage("SENDS", 0);
    mx_pair_note();
    mx_block(12, 14, "REVERB", MX_REVSEND, 0);
    gfx_bar(56, 25, 50, 6, (float)get_mx(MX_REVSEND) / 100.0f, C_ORANGE);
    mx_block(12, 48, "DELAY", MX_DLYSEND, 0);
    gfx_bar(56, 59, 50, 6, (float)get_mx(MX_DLYSEND) / 100.0f, C_ORANGE);
    mx_block(12, 82, "SEND POINT", MX_PRE, 0);
    mx_hint();
}

/* An 80 field or the LEVEL as the machine pages' blocks: title (the value
 * playing now, dim, while routed), value and bar, route. */
static void rv_block(const char *title, const char *live, const struct field *v, const struct field *src,
                     const struct field *amt, int arg, int dim)
{
    int first = nfields, y = PG_TOP + 4 + pg.y - pg.sc, lo, hi;

    field_dim = dim;
    gfx_title(12, y, title, dim ? C_DIM : C_TITLE);
    if (live) {
        gfx_text_right(127, y, live, C_DIM);
    }
    field(12, y + 11, v, arg, 0);
    v->range(arg, &lo, &hi);
    gfx_bar(80, y + 11, 46, 7, hi > lo ? (float)(v->get(arg) - lo) / (float)(hi - lo) : 0.0f, C_ORANGE);
    field(12, y + 21, src, arg, 0);
    field(46, y + 21, amt, arg, 0);
    gfx_bar_bipolar(84, y + 22, 30, 5, (float)amt->get(arg) / 100.0f, C_GREEN);
    field_dim = 0;
    pg_item(first, PG_BLOCK_H);
}

static void rv_row(const char *title, const struct field *f, int dim)
{
    int first = nfields, y = PG_TOP + 4 + pg.y - pg.sc;

    gfx_title(12, y, title, dim ? C_DIM : C_TITLE);
    field_dim = dim;
    field(12, y + 10, f, 0, 0);
    field_dim = 0;
    pg_item(first, 24);
}

static void draw_reverb(void)
{
    int algo = mix_now()->reverb_algo, chorus = mix_algo_chorus((u32)algo), prog = rv_prog_ok();

    draw_header("REVERB");
    pg_begin();
    rv_row("UNIT", &f_rvunit, 0);
    rv_row("ALGO", &f_mxalgo, 0);
    rv_row("PROGRAM", &f_rvprog, !prog);
    if (prog) {
        uint8_t edit[R70_FIELDS_MAX > R80_FIELDS_MAX ? R70_FIELDS_MAX : R80_FIELDS_MAX];
        unsigned n = rvf_fields(edit);
        char s[16];

        for (unsigned i = 0; i < n; i++) {
            int e = edit[i], v = rvf_live((unsigned)e);

            if (v >= 0 && get_rvsrc(e)) {
                fmt_rv(s, v, e);
            }
            rv_block(rvf_name((unsigned)e), v >= 0 && get_rvsrc(e) ? s : 0, &f_rv, &f_rvsrc, &f_rvamt, e, 0);
        }
    } else {
        rv_block(mx_gated() ? "GATE" : "DECAY", 0, &f_mx, &f_mxbsrc, &f_mxamt, MX_DECAY, algo == 0);
        rv_block("DAMP", 0, &f_mx, &f_mxbsrc, &f_mxamt, MX_DAMP, algo == 0);
        rv_block("RATE", 0, &f_mx, &f_mxbsrc, &f_mxamt, MX_RATE, !chorus);
        rv_block("DEPTH", 0, &f_mx, &f_mxbsrc, &f_mxamt, MX_DEPTH, !chorus);
    }
    rv_block("LEVEL", 0, &f_mx, &f_mxbsrc, &f_mxamt, MX_RLEVEL, 0);
    pg_end();
}

static const uint8_t tl_ppqns[] = { 1, 2, 4, 8, 12, 16, 24, 48, 96 };

static void fmt_bpm(char *o, uint32_t bpm10)
{
    o[0] = 0;
    fmt_fixed(o, (int)bpm10, 1, 1, 0);
    cat(o, " BPM");
}

static uint32_t dt_last_bpm = MIX_BPM_DEFAULT;
/* The Delay's tempo, as the Time Lock page's: TEMPO (BPM, MIDI CLOCK, CV
 * CLOCK), then the BPM, or the clock's input and PPQN, on a row below. */
enum { DT_SOURCE, DT_BPM, DT_INPUT, DT_PPQN };
enum { DT_OWN, DT_MIDI, DT_CV };

static const char *const dt_labels[3] = { "BPM", "MIDI CLOCK", "CV CLOCK" };

static int dt_source(void)
{
    u32 t = mix_now()->delay_tempo;

    return t == MIX_TEMPO_MIDI ? DT_MIDI : t == MIX_TEMPO_CV ? DT_CV : DT_OWN;
}

static int dt_ppqn_index(void)
{
    int p = mix_now()->delay_clk_ppqn ? mix_now()->delay_clk_ppqn : TIME_LOCK_PPQN_DEFAULT, i = 0;

    while (i + 1 < (int)sizeof(tl_ppqns) && tl_ppqns[i] < p) {
        i++;
    }
    return i;
}

static int get_dt(int f)
{
    u32 b = mix_tempo_bpm(mix_now()->delay_tempo);

    dt_last_bpm = b ? b : dt_last_bpm;
    switch (f) {
    case DT_SOURCE: return dt_source();
    case DT_BPM: return (int)dt_last_bpm;
    case DT_INPUT: return mix_now()->delay_clk_src;
    default: return dt_ppqn_index();
    }
}

static void set_dt(int f, int v)
{
    struct mix_settings *m = mix_edit();

    switch (f) {
    case DT_SOURCE:
        m->delay_tempo = v == DT_MIDI ? MIX_TEMPO_MIDI : v == DT_CV ? MIX_TEMPO_CV
                                                                    : (uint8_t)(MIX_TEMPO_BPM + dt_last_bpm - MIX_BPM_MIN);
        break;
    case DT_BPM:
        dt_last_bpm = (uint32_t)v;
        m->delay_tempo = (uint8_t)(MIX_TEMPO_BPM + v - (int)MIX_BPM_MIN);
        break;
    case DT_INPUT: m->delay_clk_src = (uint8_t)v; break;
    default: m->delay_clk_ppqn = tl_ppqns[v]; break;
    }
}

static void range_dt(int f, int *lo, int *hi)
{
    switch (f) {
    case DT_SOURCE: *lo = DT_OWN; *hi = DT_CV; break;
    case DT_BPM: *lo = (int)MIX_BPM_MIN; *hi = (int)MIX_BPM_MAX; break;
    case DT_INPUT: *lo = 0; *hi = (int)MIX_CLK_INPUTS - 1; break;
    default: *lo = 0; *hi = (int)sizeof(tl_ppqns) - 1; break;
    }
}

static void fmt_dt(char *o, int v, int f)
{
    o[0] = 0;
    switch (f) {
    case DT_SOURCE: cat(o, dt_labels[v]); break;
    case DT_BPM: fmt_int(o, v); cat(o, " BPM"); break;
    case DT_INPUT:
        if (v >= (int)MIX_CLK_INPUT_L) {
            cat(o, v == (int)MIX_CLK_INPUT_L ? "L" : "R");
        } else {
            fmt_src(o, CV_ABS + v);
        }
        break;
    default: fmt_int(o, tl_ppqns[v]); cat(o, " PPQN"); break;
    }
}

static const struct field f_dtsrc = { get_dt, set_dt, range_dt, fmt_dt, 0, L_VLIST, dt_labels, 0 };
static const struct field f_dt = { get_dt, set_dt, range_dt, fmt_dt, 0, L_NONE, 0, 0 };
static const struct field f_dtin = { get_dt, set_dt, range_dt, fmt_dt, 0, L_NONE, 0, 0 };

static int coarse_dt(int f, int v, int s)
{
    return f == DT_BPM ? coarse_step(v, s, 10, (int)MIX_BPM_MIN) : v + s;
}

/* The delay time at its tempo (a followed one that never ran: 120 BPM),
 * halved past the line's 1.37 s, as mix_fx.c. */
static int mx_delay_ms(void)
{
    static const uint8_t units[MIX_DIVISIONS] = { 12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 192 };
    const struct mix_settings *m = mix_now();
    int bpm10 = (int)mix_fx_bpm10();
    static int last10 = 1200;
    last10 = bpm10 ? bpm10 : last10;
    int frames = (int)(48000LL * 600 / last10) * units[m->delay_division < MIX_DIVISIONS ? m->delay_division : 0] / 48;

    while (frames > 65536 - 4) {
        frames >>= 1;
    }
    return frames / 48;
}

static void draw_delay(void)
{
    char s[16];
    int src = dt_source();

    draw_header("DELAY");
    gfx_title(12, 14, "TEMPO", C_TITLE);
    field(12, 24, &f_dtsrc, DT_SOURCE, 0);
    if (src == DT_OWN) {
        field(70, 24, &f_dt, DT_BPM, 0);
    } else {
        u32 bpm10 = ta_clock_bpm10();
        if (bpm10) {
            fmt_bpm(s, bpm10);
            gfx_text_right(127, 14, s, C_WHITE);
        } else {
            gfx_text_right(127, 14, "NO CLOCK", C_DIM);
        }
    }
    if (src == DT_CV) {
        field(70, 24, &f_dtin, DT_INPUT, 0);
        field(0, 24, &f_dt, DT_PPQN, 127);
    }
    mx_block(12, 33, "TIME", MX_TIME, 0);
    mx_block(70, 33, "FEEDBACK", MX_FEEDBACK, 0);
    mx_block(12, 67, "LOW CUT", MX_LOWCUT, 0);
    mx_block(70, 67, "HIGH CUT", MX_HIGHCUT, 0);
    mx_block(12, 101, "LEVEL", MX_DLEVEL, 0);
    gfx_title(70, 101, "TIME", C_DIM);
    s[0] = 0;
    fmt_int(s, mx_delay_ms());
    cat(s, " MS");
    gfx_text(70, 111, s, C_DIM);
    mx_hint();
}

static int (*mx_coarse(const struct field *f))(int, int, int)
{
    return f == &f_dt ? coarse_dt : f == &f_mx ? coarse_mx : f == &f_mxamt || f == &f_rvamt ? coarse_ten : f == &f_rv ? coarse_rv : 0;
}

/* ---- Grain and Grain CV (Tallent Edition pages, our look) ----
 * PHASE MOD -> GRAIN -> GRAIN CV -> PHASE MOD. Sample channels only (a
 * Stereo/Right channel edits its master's, which plays both sides). Hold
 * GRAIN resets the fields (On and the CV routes kept), again undoes. */

static struct grain_value gr_held;
static int gr_held_owner;

static struct grain_value *gr_cur(void) { return &preset.ch[fx_ch()].grain; }
static int get_gr(int f) { return gr_cur()->value[f]; }
static void set_gr(int f, int v) { gr_cur()->value[f] = (int16_t)v; }
static void range_gr(int f, int *lo, int *hi) { *lo = grain_ranges[f].low; *hi = grain_ranges[f].high; }

static void fmt_gr(char *o, int v, int f)
{
    o[0] = 0;
    switch (f) {
    case GRAIN_ON: cat(o, v ? "ON" : "OFF"); break;
    case GRAIN_SIZE: fmt_fixed(o, v, 1, 1, 0); cat(o, " MSEC"); break;
    case GRAIN_OVERLAP: fmt_fixed(o, v, 2, 1, 0); break;
    case GRAIN_SPRAY: fmt_int(o, v); cat(o, " MS"); break;
    case GRAIN_DETUNE: fmt_int(o, v); cat(o, " CENT"); break;
    case GRAIN_SLIDE: fmt_fixed(o, v, 1, 1, 1); cat(o, "%"); break;
    case GRAIN_SKEW: fmt_fixed(o, v, 0, 1, 1); break;
    default: fmt_int(o, v); cat(o, "%"); break;
    }
}

static void gr_defaults(struct grain_value *n)
{
    const struct grain_value *v = gr_cur();

    grain_value_defaults(n);
    n->value[GRAIN_ON] = v->value[GRAIN_ON];
    memcpy(n->source, v->source, sizeof(n->source));
    memcpy(n->amount, v->amount, sizeof(n->amount));
}

static void hold_gr(int a)
{
    struct grain_value n;

    (void)a;
    gr_defaults(&n);
    if (memcmp(&n, gr_cur(), sizeof(n))) {
        gr_held = *gr_cur();
        gr_held_owner = 1 + fx_ch();
        *gr_cur() = n;
        ui_say("GRAIN CLEARED");
    } else if (gr_held_owner == 1 + fx_ch()) {
        memcpy(gr_held.source, n.source, sizeof(n.source));
        memcpy(gr_held.amount, n.amount, sizeof(n.amount));
        gr_held.value[GRAIN_ON] = n.value[GRAIN_ON];
        *gr_cur() = gr_held;
        gr_held_owner = 0;
        ui_say("GRAIN RESTORED");
    }
}

static const struct field f_gr = { get_gr, set_gr, range_gr, fmt_gr, 0, L_NONE, 0, 0 };
static const struct field f_gron = { get_gr, set_gr, range_gr, fmt_gr, 0, L_VLIST, 0, hold_gr };

static int get_grsrc(int r) { return gr_cur()->source[r] - TA_SRC_OFF; }
static void set_grsrc(int r, int v) { gr_cur()->source[r] = (int8_t)(v + TA_SRC_OFF); }
static int get_gramt(int r) { return gr_cur()->amount[r]; }
static void set_gramt(int r, int v) { gr_cur()->amount[r] = (int8_t)v; }
static const struct field f_grsrc = { get_grsrc, set_grsrc, range_src, fmt_src_short, 0, L_CVSRC, 0, 0 };
static const struct field f_gramt = { get_gramt, set_gramt, range_amt1, fmt_amt, 0, L_NONE, 0, 0 };

/* Coarse steps (Tallent): Size doubles or halves, Detune 100 cents, Overlap
 * a quarter grain, the rest 10. */
static int coarse_gr(int f, int v, int s)
{
    if (f == GRAIN_SIZE) {
        for (; s > 0 && v < grain_ranges[f].high; s--) {
            v *= 2;
        }
        for (; s < 0 && v > grain_ranges[f].low; s++) {
            v /= 2;
        }
        return v;
    }
    return f == GRAIN_ON ? v + s : coarse_step(v, s, f == GRAIN_DETUNE ? 100 : f == GRAIN_OVERLAP ? 25 : 10, 0);
}

static int (*gr_coarse(const struct field *f))(int, int, int)
{
    return f == &f_gr || f == &f_gron ? coarse_gr : f == &f_gramt ? coarse_ten : 0;
}

/* DATA2 sweeps Size by ratio, 0.5 ms to 2 s (Tallent). */
static int gr_size_field(const struct field_ref *r) { return r->f == &f_gr && r->arg == GRAIN_SIZE; }

static int gr_size_d2(int d2)
{
    float v = (float)grain_ranges[GRAIN_SIZE].low * fast_exp2((float)d2 * (11.965784f / 4095.0f));

    return (int)(v + 0.5f);
}

static int gr_paired(void) { int m = fx_ch(); return m + 1 < CHANNELS && preset.ch[m + 1].mode == MODE_STEREO; }

static int gr_machine(const char *title)
{
    if (is_sampler(sel)) {
        return 0;
    }
    draw_header(title);
    draw_channel_column();
    gfx_text(12, 30, "SAMPLER ONLY", C_DIM);
    gfx_text(12, 44, "CHANNEL RUNS", C_DIM);
    gfx_text(12, 54, machine_name(preset.ch[sel].machine), C_WHITE);
    return 1;
}

static void gr_cell(int x, int y, const char *title, int f, int dim)
{
    gfx_title(x, y, title, dim ? C_DIM : C_TITLE);
    field_dim = dim;
    field(x, y + 10, f == GRAIN_ON ? &f_gron : &f_gr, f, 0);
    field_dim = 0;
}

static void draw_grain(void)
{
    static const struct { uint8_t f, x, y; const char *title; } cells[GRAIN_FIELDS] = {
        { GRAIN_ON, 12, 14, "GRAIN" }, { GRAIN_SIZE, 70, 14, "SIZE" },
        { GRAIN_OVERLAP, 12, 35, "OVERLAP" }, { GRAIN_SPRAY, 70, 35, "SPRAY" },
        { GRAIN_DETUNE, 12, 56, "DETUNE" }, { GRAIN_SLIDE, 70, 56, "SLIDE" },
        { GRAIN_SCAN, 12, 77, "SCAN" }, { GRAIN_PAN, 52, 77, "PAN" }, { GRAIN_SHAPE, 92, 77, "SHAPE" },
        { GRAIN_SKEW, 12, 98, "SKEW" }, { GRAIN_OCTAVE, 52, 98, "OCT" }, { GRAIN_REVERSE, 92, 98, "RVRS" },
    };
    int off;

    if (gr_machine("GRAIN")) {
        return;
    }
    spage("GRAIN", 0);
    mx_pair_note();
    off = !gr_cur()->value[GRAIN_ON];
    for (int i = 0; i < GRAIN_FIELDS; i++) {
        int f = cells[i].f;
        gr_cell(cells[i].x, cells[i].y, cells[i].title, f, f != GRAIN_ON && (off || (f == GRAIN_PAN && !gr_paired())));
    }
    if (focus[P_GRAIN] < nfields && fields[focus[P_GRAIN]].f == &f_gron) {
        struct grain_value n;
        gr_defaults(&n);
        if (memcmp(&n, gr_cur(), sizeof(n))) {
            fx_hint("HOLD: CLEAR");
        } else if (gr_held_owner == 1 + fx_ch()) {
            fx_hint("HOLD: RESTORE");
        }
    }
}

static void draw_graincv(void)
{
    static const char *const names[GRAIN_ROUTES] = { "SIZE", "OVERLAP", "SPRAY", "DETUNE", "SLIDE", "SCAN", "SHAPE",
                                                     "PAN" };
    int f = focus[P_GRAINCV], off;

    if (gr_machine("GRAIN CV")) {
        return;
    }
    spage("GRAIN CV", 0);
    mx_pair_note();
    off = !gr_cur()->value[GRAIN_ON];
    for (int r = 0; r < GRAIN_ROUTES; r++) {
        int y = 16 + r * 12;
        gfx_text(12, y, names[r], off ? C_DIM : C_TITLE);
        field_dim = off;
        field(60, y, &f_grsrc, r, 0);
        field(84, y, &f_gramt, r, 0);
        field_dim = 0;
        gfx_bar_bipolar(116, y + 1, 11, 5, (float)gr_cur()->amount[r] / 100.0f, C_GREEN);
    }
    /* What a volt does at the focused amount: 5 V cross the range, or for
     * Size and Overlap five octaves. */
    if (f / 2 < GRAIN_ROUTES && gr_cur()->source[f / 2] != TA_SRC_OFF) {
        int r = f / 2, a = gr_cur()->amount[r], span = grain_routes[r].span;
        if (!span) {
            per_volt_hint(a, 2, "OCT");
        } else if (r == GRAIN_ROUTE_SLIDE) {
            per_volt_hint(a * span / 500, 1, "%");
        } else {
            per_volt_hint(a * span / 500, 0, r == GRAIN_ROUTE_SPRAY ? "MS" : r == GRAIN_ROUTE_DETUNE ? "CENTS" : "%");
        }
    }
}

/* ---- Time Lock (Tallent Edition page, our look) ----
 * PITCH -> TIME LOCK -> PITCH. Sample channels only (a Stereo/Right
 * channel edits its master's, which locks the pair). TEMPO follows channel
 * 1 (CH 1, not on channel 1), the loop's own tempo, a SPEED of it, MIDI
 * clock (no MIDI input yet: NO CLOCK) or a CV CLOCK (its input and pulses a
 * beat; CV A-C are the channel's). BEATS is the selected zone's loop
 * length, AUTO from the sample's name or 90-180 BPM. Holds (DATA1): TEMPO
 * back to CH 1, SPEED to 100%, BEATS to AUTO. */
enum { TL_ON, TL_TEMPO, TL_SPEED, TL_INPUT, TL_PPQN, TL_BEATS };

static const char *const tl_tempo_labels[TIME_LOCK_TEMPOS] = { "CH 1", "OWN", "SPEED", "MIDI CLOCK", "CV CLOCK" };
static const char *const tl_on_labels[2] = { "OFF", "ON" };

static struct timelock_value *tl_cur(void) { return &preset.ch[fx_ch()].tl; }
static int tl_first(void) { return fx_ch() ? TIME_LOCK_TEMPO_CHANNEL_1 : TIME_LOCK_TEMPO_OWN; }
static int tl_tempo(void) { return !fx_ch() && !tl_cur()->tempo ? TIME_LOCK_TEMPO_OWN : tl_cur()->tempo; }

static int tl_ppqn_index(void)
{
    int p = tl_cur()->ppqn ? tl_cur()->ppqn : TIME_LOCK_PPQN_DEFAULT, i = 0;

    while (i + 1 < (int)sizeof(tl_ppqns) && tl_ppqns[i] < p) {
        i++;
    }
    return i;
}

static int get_tl(int f)
{
    struct tlock_tempo t;

    switch (f) {
    case TL_ON: return tl_cur()->on;
    case TL_TEMPO: return tl_tempo();
    case TL_SPEED: return tl_cur()->speed;
    case TL_INPUT: return tl_cur()->input - TA_SRC_OFF;
    case TL_PPQN: return tl_ppqn_index();
    default:
        /* From AUTO the guess is where turning starts. */
        tlock_tempo(fx_ch(), cur_zone(), &t);
        return (int)t.beats;
    }
}

static void set_tl(int f, int v)
{
    struct timelock_value *t = tl_cur();

    switch (f) {
    case TL_ON: t->on = (uint8_t)v; break;
    case TL_TEMPO: t->tempo = (uint8_t)v; break;
    case TL_SPEED: t->speed = (int16_t)v; break;
    case TL_INPUT: t->input = (int8_t)(v + TA_SRC_OFF); break;
    case TL_PPQN: t->ppqn = tl_ppqns[v]; break;
    default: zcfg(fx_ch(), cur_zone())->beats = (uint8_t)v; break;
    }
}

static void range_tl(int f, int *lo, int *hi)
{
    switch (f) {
    case TL_ON: *lo = 0; *hi = 1; break;
    case TL_TEMPO: *lo = tl_first(); *hi = TIME_LOCK_TEMPOS - 1; break;
    case TL_SPEED: *lo = TIME_LOCK_SPEED_MIN; *hi = TIME_LOCK_SPEED_MAX; break;
    case TL_INPUT: range_src(0, lo, hi); break;
    case TL_PPQN: *lo = 0; *hi = (int)sizeof(tl_ppqns) - 1; break;
    default: *lo = 0; *hi = TIME_LOCK_BEATS_MAX; break;
    }
}

static void fmt_tl(char *o, int v, int f)
{
    o[0] = 0;
    switch (f) {
    case TL_ON: cat(o, tl_on_labels[v & 1]); break;
    case TL_TEMPO: cat(o, tl_tempo_labels[v]); break;
    case TL_SPEED: fmt_fixed(o, TIME_LOCK_SPEED_UNITY + v, 1, 1, 0); cat(o, "%"); break;
    case TL_INPUT: fmt_src(o, v); break;
    case TL_PPQN: fmt_int(o, tl_ppqns[v]); cat(o, " PPQN"); break;
    default:
        if (!v) {
            cat(o, "AUTO");
            break;
        }
        if (!zcfg(fx_ch(), cur_zone())->beats) {
            cat(o, "AUTO ");
        }
        fmt_int(o + strlen(o), v);
        break;
    }
}

static void hold_tl(int f)
{
    struct timelock_value *t = tl_cur();
    struct zone_cfg *z = zcfg(fx_ch(), cur_zone());

    if (f == TL_BEATS && z->beats) {
        z->beats = 0;
        ui_say("BEATS AUTO");
    } else if (f == TL_SPEED && t->speed) {
        t->speed = 0;
        ui_say("SPEED 100%");
    } else if (f == TL_TEMPO && fx_ch() && t->tempo != TIME_LOCK_TEMPO_CHANNEL_1) {
        t->tempo = TIME_LOCK_TEMPO_CHANNEL_1;
        ui_say("TEMPO CH 1");
    }
}

static const struct field f_tlon = { get_tl, set_tl, range_tl, fmt_tl, 0, L_VLIST, tl_on_labels, 0 };
static const struct field f_tltempo = { get_tl, set_tl, range_tl, fmt_tl, 0, L_VLIST, tl_tempo_labels, hold_tl };
static const struct field f_tl = { get_tl, set_tl, range_tl, fmt_tl, 0, L_NONE, 0, hold_tl };
static const struct field f_tlsrc = { get_tl, set_tl, range_tl, fmt_tl, 0, L_CVSRC, 0, 0 };

/* Coarse (Tallent): Speed 1%, Beats doubles or halves (below 1: AUTO). */
static int coarse_tl(int f, int v, int s)
{
    if (f == TL_BEATS) {
        for (; s > 0 && v < (int)TIME_LOCK_BEATS_MAX; s--) {
            v = v ? v * 2 : 1;
        }
        for (; s < 0 && v > 0; s++) {
            v /= 2;
        }
        return v;
    }
    return f == TL_SPEED ? coarse_step(v, s, 10, 0) : v + s;
}

static int (*tl_coarse(const struct field *f))(int, int, int)
{
    return f == &f_tl ? coarse_tl : 0;
}

static void draw_tlock(void)
{
    struct tlock_tempo t;
    char s[20];
    int c = fx_ch(), off, tempo_off, tempo = tl_tempo(), f = focus[P_TLOCK];

    if (gr_machine("TIME LOCK")) {
        return;
    }
    spage("TIME LOCK", 1);
    mx_pair_note();
    tlock_tempo(c, cur_zone(), &t);
    off = !tl_cur()->on;
    tempo_off = off && c;           /* channel 1's tempo serves the others */
    gfx_title(12, 14, "TIME LOCK", C_TITLE);
    field(12, 24, &f_tlon, TL_ON, 0);
    if (!off) {
        int m = tlock_mapped(c, cur_zone());
        gfx_text(52, 24, m < 0 ? "NO SAMPLE" : m ? (preset.ch[c].s.reverse ? "NOT IN REVERSE" :
                                                       fx_grain(c) ? "GRAIN MODE" : "LOCKED") : "FINDING HITS",
                 m > 0 && !preset.ch[c].s.reverse && !fx_grain(c) ? C_GREEN : C_DIM);
    }
    gfx_title(12, 40, "TEMPO", tempo_off ? C_DIM : C_TITLE);
    field_dim = tempo_off;
    field(12, 50, &f_tltempo, TL_TEMPO, 0);
    if (t.bpm10) {
        fmt_bpm(s, t.bpm10);
        gfx_text_right(127, 50, s, tempo_off ? C_DIM : C_WHITE);
    } else if (t.clocked) {
        gfx_text_right(127, 50, "NO CLOCK", C_DIM);
    }
    if (tempo == TIME_LOCK_TEMPO_SPEED) {
        field(12, 61, &f_tl, TL_SPEED, 0);
    } else if (tempo == TIME_LOCK_TEMPO_CV) {
        field(12, 61, &f_tlsrc, TL_INPUT, 0);
        field(0, 61, &f_tl, TL_PPQN, 127);
    }
    field_dim = off;
    gfx_title(12, 78, "BEATS", off ? C_DIM : C_TITLE);
    field(12, 88, &f_tl, TL_BEATS, 0);
    if (t.own_bpm10) {
        fmt_bpm(s, t.own_bpm10);
        gfx_text_right(127, 88, s, C_DIM);
    }
    field_dim = 0;
    if (f < nfields && fields[f].f == &f_tltempo && c && tl_cur()->tempo != TIME_LOCK_TEMPO_CHANNEL_1) {
        fx_hint("HOLD: FOLLOW CH 1");
    } else if (f < nfields && fields[f].f == &f_tl && fields[f].arg == TL_SPEED && tl_cur()->speed) {
        fx_hint("HOLD: 100%");
    } else if (f < nfields && fields[f].f == &f_tl && fields[f].arg == TL_BEATS && zcfg(c, cur_zone())->beats) {
        fx_hint("HOLD: AUTO");
    }
}

/* Waveform of the selected zone with its two points: an overview (zoom
 * 1-32x around the point being edited) and a frame-by-frame view (on the
 * loop page: the loop's end on the left, its start on the right, the
 * splice in the middle). Reversed channels show mirrored, in a red frame. */
#define WV_X    12
#define WV_W    116

static void wave_plot(int y0, int h, const int16_t *lo, const int16_t *hi, int rev, int64_t ra, int64_t rb,
                      int64_t a, int64_t span)
{
    int peak = 64, mid = y0 + h / 2, half = h / 2 - 1;

    for (int c = 0; c < WV_W; c++) {
        int m = hi[c] > -lo[c] ? hi[c] : -lo[c];
        peak = m > peak ? m : peak;
    }
    for (int c = 0; c < WV_W; c++) {
        int64_t f = a + span * c / WV_W;
        int t = mid - hi[c] * half / peak, b = mid - lo[c] * half / peak;
        int x = WV_X + (rev ? WV_W - 1 - c : c);
        gfx_rect(x, t, 1, b - t + 1, f >= ra && f < rb ? C_WHITE : C_DIM);
    }
}

static int wave_x(int64_t f, int64_t a, int64_t span, int rev)
{
    int64_t c = (f - a) * WV_W / span;

    if (c < 0 || c >= WV_W) {
        return -1;
    }
    return WV_X + (rev ? WV_W - 1 - (int)c : (int)c);
}

static void draw_pos(int loop)
{
    int zi = cur_zone(), rev = preset.ch[sel].s.reverse, second = focus[page] == 1 || focus[page] >= 4;
    const struct zone_cfg *z = zcfg(sel, zi);
    const struct sample *m = zone_smp(sel, zi);
    int64_t fr = zone_frames(sel, zi), p0, p1;
    static int16_t lo[WV_W], hi[WV_W];
    char s[24];

    spage(loop ? (preset.ch[sel].s.loop_is_end ? "LOOP S/END" : "LOOP S/LEN") : "START/END", 1);
    if (!zone_count(sel)) {
        gfx_text(12, 40, "NO SAMPLE", C_DIM);
        return;
    }
    field(12, 15, &f_pos, loop ? PF_LSTART : PF_START, 0);
    field(0, 15, &f_pos, loop ? PF_LEND : PF_END, 127);
    gfx_text(12, 84, "START MOD", C_TITLE);
    gfx_text(76, 84, loop && !preset.ch[sel].s.loop_is_end ? "LEN MOD" : "END MOD", C_TITLE);
    field(12, 95, &f_ssrc, loop ? SM_LSTART : SM_SSTART, 0);
    field(34, 95, &f_samt, loop ? SM_LSTART : SM_SSTART, 0);
    field(76, 95, &f_ssrc, loop ? SM_LLEN : SM_SEND, 0);
    field(98, 95, &f_samt, loop ? SM_LLEN : SM_SEND, 0);
    if (loop) {
        p0 = z->loop_start;
        p1 = z_lend32(z, (uint32_t)fr) / 32;
    } else {
        p0 = z->start;
        p1 = z_end(z, (uint32_t)fr);
    }
    gfx_frame(WV_X - 1, 26, WV_W + 1, 28, rev ? C_RED : C_FAINT);
    gfx_frame(WV_X - 1, 59, WV_W + 1, 24, rev ? C_RED : C_FAINT);
    if (!m || m->state != SMP_READY || fr < 1) {
        gfx_text(20, 36, !m ? "NO SAMPLE" : m->state == SMP_MISSING ? "MISSING" : m->state == SMP_BAD ? "BAD WAV" :
                 m->state == SMP_NOMEM ? "NO MEMORY" : "LOADING", C_DIM);
    } else {
        int side = z->side & 1;
        int64_t span = fr / pos_zoom, pt = second ? p1 : p0, a;

        span = span < WV_W ? (fr < WV_W ? fr : WV_W) : span;
        a = pt - span / 2;
        a = a > fr - span ? fr - span : a;
        a = a < 0 ? 0 : a;
        if (wave_columns(m, side, a, a + span, WV_W, lo, hi)) {
            int x;
            wave_plot(27, 26, lo, hi, rev, p0, p1, a, span);
            if ((x = wave_x(p0, a, span, rev)) >= 0) {
                gfx_rect(x, 27, 1, 26, C_GREEN);
            }
            if ((x = wave_x(p1 - (p1 > 0), a, span, rev)) >= 0) {
                gfx_rect(x, 27, 1, 26, C_RED);
            }
        } else {
            gfx_text(20, 36, "READING...", C_DIM);
        }
        /* Zoom bar: the part of the sample shown. */
        gfx_rect(WV_X + (int)(a * WV_W / fr), 55, (int)(span * WV_W / fr) + 1, 2, C_TITLE);
        /* Frame by frame. */
        for (int c = 0; c < WV_W; c++) {
            int64_t f = loop ? (c < WV_W / 2 ? p1 - WV_W / 2 + c : p0 + c - WV_W / 2) : pt - WV_W / 2 + c;
            lo[c] = hi[c] = (int16_t)wave_at(m, side, f);
        }
        for (int c = 0; c + 1 < WV_W; c++) {
            int16_t l = lo[c] < lo[c + 1] ? lo[c] : lo[c + 1], h = hi[c] > hi[c + 1] ? hi[c] : hi[c + 1];
            lo[c] = l;
            hi[c] = h;
        }
        wave_plot(60, 22, lo, hi, rev, INT64_MIN, INT64_MAX, 0, WV_W);
        gfx_rect(WV_X + WV_W / 2, 60, 1, 22, loop || second ? C_RED : C_GREEN);
    }
    s[0] = 0;
    if (pos_zoom > 1) {
        cat(s, "X");
        put_uint(s + 1, (unsigned)pos_zoom, 1);
    }
    gfx_text(12, 120, s, C_TITLE);
    if (m) {
        copy_n(s, m->name, 14);
        gfx_text_right(127, 120, s, C_FAINT);
    }
}

static void draw_zones(void)
{
    int n = zone_count(sel), cz = cur_zone(), sr = preset_param_channel(sel) != sel;
    int top = 16, bot = 104;
    const struct sampler_cfg *s = &preset.ch[preset_param_channel(sel)].s;
    char t[24];

    spage(sr ? "ZONES (R)" : "ZONES", 1);
    /* Voltage bar, +5 V at the top: boundaries, the selected zone, the CV. */
    gfx_frame(13, top, 5, bot - top + 1, C_DIM);
    for (int z = 0; z < n; z++) {
        int y0 = z ? top + (500 - zcfg(sel, z - 1)->minv) * (bot - top) / 1000 : top;
        int y1 = z + 1 < n ? top + (500 - zcfg(sel, z)->minv) * (bot - top) / 1000 : bot;
        if (z == cz) {
            gfx_rect(14, y0, 3, y1 - y0 + 1, C_TITLE);
        }
        if (z + 1 < n) {
            gfx_rect(12, y1, 7, 1, C_WHITE);
        }
    }
    if (s->zones_cv && n > 1) {
        float v = engine_cv(s->zones_cv, preset_param_channel(sel));
        int y = top + (int)((5.0f - v) * (float)(bot - top) / 10.0f);
        gfx_rect(19, y < top ? top : y > bot ? bot : y, 2, 1, C_YELLOW);
    }
    for (int z = 0; z <= n && z < ZONES; z++) {
        int y = top + z * 10, foc = field_reg(&f_zsmp, sel * ZONES + z);
        const struct sample *m = z < n ? zone_smp(sel, z) : 0;
        if (z < n) {
            char *dot;
            copy_n(t, m ? m->name : "?", 13);
            if ((dot = strchr(t, '.'))) {
                *dot = 0;
            }
        } else {
            memcpy(t, "++", 3);
        }
        if (foc) {
            gfx_text_box(24, y, t, C_BLACK, fine ? C_ORANGE : C_WHITE);
            if (z < n && z != zsel[sel]) {
                zone_select(sel, z);
            }
        } else {
            gfx_text(24, y, t, m && m->state != SMP_READY ? C_DIM : C_WHITE);
        }
        if (z < n && (engine_status.awake >> sel) & 1 && engine_status.zone[sel] == z) {
            gfx_rect(21, y + 2, 2, 3, C_RED);
        }
        if (z + 1 < n) {
            if (field(0, y, &f_zvolt, z, 127)) {
                zone_select(sel, z);
            }
        } else if (z < n) {
            gfx_text_right(127, y, "-5.00", C_FAINT);
        }
    }
    if (sr) {
        memcpy(t, "ZONE CV: CH ", 13);
        t[12] = (char)('1' + preset_param_channel(sel));
        t[13] = 0;
        gfx_text(12, 110, t, C_FAINT);
    } else {
        gfx_title(12, 110, "CV", C_TITLE);
        field(30, 110, &f_zcv, 0, 0);
        field(0, 110, &f_zrt, 0, 127);
    }
}

enum { UK_PAGE, UK_INIT, UK_NEWFOLDER, UK_FUTIL, UK_WDT };
struct util_item {
    const char *label;
    uint8_t kind, card;
    int page;
};
/* Grouped: preset and setup, files and firmware, information, diagnostics.
 * TEST WATCHDOG stays last (dev builds only). */
static const struct util_item util_items[] = {
    { "PREFERENCES", UK_PAGE, 0, P_PREFS },
    { "INC/DEC PRESET", UK_PAGE, 0, P_INCDEC },
    { 0, UK_INIT, 0, 0 },
    { "CALIBRATE", UK_PAGE, 0, P_CAL },
    { "NEW FOLDER", UK_NEWFOLDER, 1, 0 },
    { "FOLDER UTILITIES", UK_FUTIL, 1, 0 },
    { "LOAD SOFTWARE", UK_PAGE, 1, P_SOFTWARE },
    { "MEMORY USAGE", UK_PAGE, 0, P_MEMORY },
    { "SD CARD USAGE", UK_PAGE, 0, P_SDCARD },
    { "AUDIO HEALTH", UK_PAGE, 0, P_AUDIO },
    { "CV MONITOR", UK_PAGE, 0, P_CVMON },
    { "ABOUT", UK_PAGE, 0, P_ABOUT },
    { "PANEL TEST", UK_PAGE, 0, P_PANEL },
    { "MACHINE BENCH", UK_PAGE, 0, P_BENCH },
    { "TEST WATCHDOG", UK_WDT, 0, 0 },
};
#define UTIL_ALL ((int)(sizeof(util_items) / sizeof(util_items[0])))
#define UTIL_ITEMS (FW_RELEASE ? UTIL_ALL - 1 : UTIL_ALL)
#define UTIL_ROWS 10

static void act_testwdt(int a)
{
    static const int items[] = { M_CANCEL, M_HANG };

    (void)a;
    menu_open("TEST WATCHDOG?", items, 2);
}
static const struct field f_testwdt = { 0, 0, 0, 0, act_testwdt, L_NONE, 0, 0 };

static void draw_util(void)
{
    int top = focus[P_UTIL] - UTIL_ROWS / 2;

    top = top > UTIL_ITEMS - UTIL_ROWS ? UTIL_ITEMS - UTIL_ROWS : top;
    top = top < 0 ? 0 : top;
    gfx_title(0, 2, "UTILITIES", C_TITLE);
    for (int i = 0; i < UTIL_ITEMS; i++) {
        const struct util_item *u = &util_items[i];
        int y = 16 + (i - top) * 11, foc;
        const char *label = u->label ? u->label : init_armed ? "CLICK TO CONFIRM" : "INIT PRESET";
        switch (u->kind) {
        case UK_WDT: foc = field_reg(&f_testwdt, 0); break;
        case UK_INIT: foc = field_reg(&f_init, 0); break;
        case UK_NEWFOLDER: foc = field_reg(&f_ufolder, 0); break;
        case UK_FUTIL: foc = field_reg(&f_ufolder, 1); break;
        default: foc = field_reg(&f_open, u->page); break;
        }
        if (i < top || i >= top + UTIL_ROWS) {
            continue;
        }
        if (foc) {
            gfx_rect(0, y - 2, 128, 11, C_SELBAR);
        }
        if (u->card && !storage_card()) {
            gfx_text(3, y, u->kind == UK_FUTIL ? "FOLDER UTILS" : label, C_DIM);
            gfx_text_right(126, y, "NO CARD", C_DIM);
            continue;
        }
        gfx_text(3, y, label, C_WHITE);
        gfx_text_right(126, y, "...", C_WHITE);
    }
}

static void draw_cvmon(void)
{
    char s[12];

    gfx_title(0, 2, cv_raw_view ? "CV RAW" : "CV MONITOR", C_TITLE);
    field_reg(&f_raw, 0);
    if (cv_raw_view) {
        gfx_text(16, 14, "ADC0", C_DIM);
        gfx_text(76, 14, "ADC1", C_DIM);
        for (int ch = 0; ch < 12; ch++) {
            int y = 24 + ch * 8;
            put_uint(s, (unsigned)ch, 2);
            gfx_text(0, y, s, C_DIM);
            put_uint(s, cv_raw[0][ch], 4);
            gfx_text(16, y, s, C_WHITE);
            put_uint(s, cv_raw[1][ch], 4);
            gfx_text(76, y, s, C_WHITE);
        }
    } else {
        gfx_text(16, 14, "A", C_RED);
        gfx_text(54, 14, "B", C_GREEN);
        gfx_text(92, 14, "C", 0x3a7fu);
        for (int ch = 0; ch < CHANNELS; ch++) {
            int y = 24 + ch * 12;
            char n[2] = { (char)('1' + ch), 0 };
            gfx_text(0, y, n, C_TITLE);
            for (int k = 0; k < 3; k++) {
                float v = cv_volts[ch * 3 + k];
                int mv = (int)(v * 100.0f + (v < 0 ? -0.5f : 0.5f));
                fmt_fixed(s, mv, 2, 1, 1);
                gfx_text(10 + k * 38, y, s, C_WHITE);
                gfx_bar_bipolar(10 + k * 38, y + 8, 34, 3, v / 5.0f, C_GREEN);
            }
        }
    }
    if (cv_warning) {
        gfx_text(0, 121, cv_warning, C_RED);
        return;
    }
    if (cv_raw_view && cv_chan_id_ok) {
        for (int a = 0; a < 2; a++) {
            uint16_t bad = cv_row_bad[a];
            put_uint(s, cv_row_rot[a], 2);
            s[2] = ' ';
            for (int d = 0; d < 4; d++) {
                s[3 + d] = "0123456789ABCDEF"[bad >> (12 - 4 * d) & 15];
            }
            s[7] = 0;
            gfx_text(a ? 76 : 16, 121, s, bad ? C_RED : C_DIM);
        }
        return;
    }
    gfx_text(0, 121, cv_chan_id_ok ? "IDS OK" : "NO IDS", cv_chan_id_ok ? C_DIM : C_YELLOW);
    gfx_text_right(127, 121, "CLICK: RAW", C_FAINT);
}

static void draw_panel(void)
{
    const struct panel *p = last_panel;
    static int enc_total;
    char s[16];
    int y = 50;

    gfx_title(0, 2, "PANEL TEST", C_TITLE);
    if (!p) {
        return;
    }
    enc_total += p->data1_steps;
    gfx_text(0, 16, "DATA1", C_DIM);
    fmt_int(s, enc_total);
    gfx_text(40, 16, s, C_WHITE);
    gfx_text(0, 26, "DATA2", C_DIM);
    fmt_int(s, p->data2);
    gfx_text(40, 26, s, C_WHITE);
    gfx_bar(70, 26, 57, 7, (float)p->data2 / 4095.0f, C_GREEN);
    gfx_text(0, 38, "GATES", C_DIM);
    for (int i = 0; i < 8; i++) {
        char c[2] = { (char)('1' + i), 0 };
        gfx_text(40 + i * 10, 38, c, (engine_status.gates >> i) & 1 ? C_GREEN : C_FAINT);
    }
    gfx_text(0, y, "HELD", C_DIM);
    for (int b = 0; b < BTN_COUNT && y < 120; b++) {
        if (p->held & (1u << b)) {
            gfx_text(40, y, panel_button_name(b), C_WHITE);
            y += 9;
        }
    }
}

/* MPU clock as MLO left it (stock never changes it): 24 MHz * M / (N + 1)
 * / M2 when the DPLL is locked, else 0. */
static uint32_t mpu_mhz(void)
{
    uint32_t clksel = REG(0x44e0042cu), m2 = REG(0x44e004a8u) & 0x1fu;
    uint32_t mode = REG(0x44e00488u) & 7u;
    uint32_t m = (clksel >> 8) & 0x7ffu, n = clksel & 0x7fu;

    if (mode != 7 || !m2) {
        return 0;
    }
    return 24u * m / (n + 1u) / m2;
}

static void hex(char *out, uint32_t v, int digits)
{
    for (int i = 0; i < digits; i++) {
        out[i] = "0123456789ABCDEF"[(v >> (4 * (digits - 1 - i))) & 15];
    }
    out[digits] = 0;
}

static void stat_line(int x, int y, const char *name, uint32_t v, int warn)
{
    char s[12];

    gfx_text(x, y, name, C_DIM);
    fmt_int(s, (int)v);
    gfx_text_right(x + 60, y, s, warn && v ? C_YELLOW : C_WHITE);
}

static void act_hlog(int a) { (void)a; health_request(); }
static const struct field f_hlog = { 0, 0, 0, 0, act_hlog, L_NONE, 0, 0 };

/* One screen, no scrolling (read off by hand); click DATA1 logs a line to
 * the card (health.c). */
static void draw_audio(void)
{
    uint32_t cpu = engine_status.render_us_last * 100u / 667u;
    struct prof_view pv;
    char n[12];

    field_reg(&f_hlog, 0);
    gfx_title(0, 0, "AUDIO HEALTH", C_TITLE);
    {
        char lg[16] = "LOG ";
        fmt_int(n, (int)health_lines());
        cat(lg, n);
        gfx_text_right(127, 0, lg, C_FAINT);
    }
    static const char *const names[14] = { "LATE", "REP", "UNDER", "MISS", "ISR", "SLOW", "RENDER", "MAX",
                                           "CPU %", "OVERLD", "LOOP MS", "IN LOST", "M1 RST", "FAIL" };
    /* M1 RST, FAIL: the McASP1 watchdog (its last stall's state below). */
    const uint32_t vals[14] = { audio_stats.late_max, audio_stats.repeats, audio_stats.underruns,
                                audio_stats.missed, audio_stats.isr_us_max, diag_slow_loops,
                                engine_status.render_us_last, engine_status.render_us_max, cpu,
                                engine_status.overloads, loop_us_max_seen / 1000u, audio_stats.in_lost,
                                audio_stats.m1_restarts, audio_stats.m1_restart_fail };
    const uint32_t warn = 0x3a2eu;     /* bit per count that should stay 0 */
    for (int i = 0; i < 14; i++) {
        stat_line((i & 1) * 66, 8 + (i / 2) * 8, names[i], vals[i], (int)((warn >> i) & 1u));
    }
    gfx_text(0, 64, "AWAKE", C_DIM);
    for (int i = 0; i < 8; i++) {
        char c[2] = { (char)('1' + i), 0 };
        gfx_text(40 + i * 10, 64, c, (engine_status.awake >> i) & 1 ? C_GREEN : C_FAINT);
    }
    /* Stage times, us: worst block and mean over the last ~2.7 s. */
    prof_get(&pv);
    gfx_rect(0, 71, 128, 1, C_FAINT);
    for (int x = 0; x < 128; x += 66) {
        gfx_text(x, 72, "US", C_FAINT);
        gfx_text_right(x + 39, 72, "MAX", C_FAINT);
        gfx_text_right(x + 60, 72, "AVG", C_FAINT);
    }
    for (int s = 0; s < PS_SHOWN; s++) {
        int x = (s / 5) * 66, y = 80 + (s % 5) * 8, i = s == PS_REST ? PS_RST_ALL : s;
        uint32_t mx = pv.max_us[i];
        gfx_text(x, y, s == PS_REST ? "RST" : prof_names[s], C_DIM);
        fmt_int(n, (int)mx);
        gfx_text_right(x + 39, y, n, mx > 300 ? C_ORANGE : C_WHITE);
        fmt_int(n, (int)((pv.mean_us10[i] + 5u) / 10u));
        gfx_text_right(x + 60, y, n, C_DIM);
    }
    if (audio_warning) {
        gfx_text(0, 120, audio_warning, C_RED);
    } else if (!panel_gate_irq_on()) {
        gfx_text(0, 120, "GATES POLLED", C_YELLOW);
    } else if (!audio_stats.m1_restarts) {
        /* CV resyncs, read lag, scans per frame, worst deviation */
        char line[32] = "R", v[12];
        fmt_int(v, (int)cv_resyncs);
        cat(line, v);
        cat(line, " L");
        fmt_int(v, (int)cv_read_lag);
        cat(line, v);
        cat(line, " S");
        fmt_fixed(v, (int)(cv_scan_rate * 1000.0f + 0.5f), 3, 1, 0);
        cat(line, v);
        cat(line, " D");
        fmt_int(v, (int)cv_dev_min);
        cat(line, v);
        gfx_text(0, 120, line, C_FAINT);
    } else {
        char line[24] = "", h[8];
        hex(h, audio_stats.m1_stall_xstat & 0xfff, 3);
        cat(line, "X");
        cat(line, h);
        hex(h, audio_stats.m1_stall_rstat & 0xfff, 3);
        cat(line, " R");
        cat(line, h);
        hex(h, audio_stats.m1_stall_gblctl & 0xffff, 4);
        cat(line, " G");
        cat(line, h);
        hex(h, audio_stats.m1_stall_edma & 0xffff, 4);
        cat(line, " E");
        cat(line, h);
        gfx_text(0, 120, line, C_YELLOW);
    }
}

/* Render time per 32-frame block (667 us) with only that machine playing;
 * BASE is with no machine (CV scan, mixing). */
static void draw_bench(void)
{
    char s[12];

    field_reg(&f_bench, 0);
    if (bench_voice < 0 && !bench_done) {
        gfx_title(0, 2, "MACHINE BENCH", C_TITLE);
        gfx_text(0, 20, "CLICK TO RUN: EACH", C_WHITE);
        gfx_text(0, 30, "MACHINE ALONE ON CH1,", C_WHITE);
        gfx_text(0, 40, "RENDER US PER BLOCK", C_WHITE);
        gfx_text(0, 50, "OF 667 US. ~23 S.", C_WHITE);
        gfx_text(0, 64, "PRESET IS RESTORED.", C_DIM);
        return;
    }
    gfx_text(0, 0, "BENCH US", C_TITLE);
    if (bench_voice != 0) {
        gfx_text(64, 0, "BASE", C_DIM);
        put_uint(s, bench_us[0], 1);
        gfx_text_right(126, 0, s, C_WHITE);
    }
    for (int v = 1; v < VOICE_COUNT; v++) {
        int x = ((v - 1) / 15) * 65, y = 9 + ((v - 1) % 15) * 8;
        uint32_t us = bench_us[v];
        gfx_text(x, y, voices[v]->name, v == bench_voice ? C_YELLOW : C_TITLE);
        if (v < bench_voice || bench_done) {
            put_uint(s, us, 1);
            gfx_text_right(x + 62, y, s, us > 600 ? C_RED : us > 300 ? C_ORANGE : C_WHITE);
        }
    }
}

/* KB as "123 KB", "12.3 MB" or "12.3 GB". */
static void fmt_kb(char *o, uint32_t kb)
{
    if (kb < 10240u) {
        put_uint(o, kb, 1);
        cat(o, " KB");
    } else if (kb < 10u * 1048576u) {
        fmt_fixed(o, (int)(kb * 10u / 1024u), 1, 1, 0);
        cat(o, " MB");
    } else {
        fmt_fixed(o, (int)((kb / 1024u) * 10u / 1024u), 1, 1, 0);
        cat(o, " GB");
    }
}

static void usage_row(int y, const char *name, uint32_t kb)
{
    char s[16];

    gfx_text(0, y, name, C_DIM);
    fmt_kb(s, kb);
    gfx_text_right(127, y, s, C_WHITE);
}

static void usage_bar(int y, uint32_t used, uint32_t total)
{
    gfx_bar(0, y, 127, 9, total ? (float)used / (float)total : 0.0f, C_ORANGE);
}

static int mem_sel;
static int get_msel(int a) { (void)a; return mem_sel < samples_count() ? mem_sel : 0; }
static void set_msel(int a, int v) { (void)a; mem_sel = v; }
static void range_msel(int a, int *lo, int *hi) { (void)a; *lo = 0; *hi = samples_count() ? samples_count() - 1 : 0; }
static void fmt_msel(char *o, int v, int a)
{
    (void)a;
    if (!samples_count()) {
        o[0] = 0;
        cat(o, "NO WAVS");
        return;
    }
    copy_n(put_uint(o, (unsigned)v + 1u, 1), " ", 1);
    copy_n(o + strlen(o), samples[v].name, 17);
}
static const struct field f_msel = { get_msel, set_msel, range_msel, fmt_msel, 0, L_NONE, 0, 0 };

static void put_hex(char *o, uint32_t v)
{
    for (int i = 0; i < 8; i++) {
        o[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15u];
    }
    o[8] = 0;
}

static void draw_memory(void)
{
    static const char *const states[] = { "FOUND", "HEADER", "LOADING", "READY", "NO MEMORY", "BAD", "MISSING" };
    uint32_t used = samples_pool_used() / 1024u, total = samples_pool_size() / 1024u;
    uint32_t us = samples_read_us();
    int ready = 0;
    char s[32];

    for (int i = 0; i < samples_count(); i++) {
        ready += samples[i].state == SMP_READY;
    }
    gfx_title(0, 2, "MEMORY USAGE", C_TITLE);
    usage_row(16, "SAMPLES USED", used);
    usage_row(26, "TOTAL", total);
    usage_bar(37, used, total);
    s[0] = 0;
    put_uint(s, (unsigned)ready, 1);
    cat(s, " OF ");
    put_uint(s + strlen(s), (unsigned)samples_count(), 1);
    cat(s, " WAVS IN RAM");
    gfx_text(0, 49, s, C_WHITE);
    s[0] = 0;
    if (samples_nomem()) {
        put_uint(s, (unsigned)samples_nomem(), 1);
        cat(s, " NO MEMORY ");
    }
    if (samples_bad()) {
        put_uint(s + strlen(s), (unsigned)samples_bad(), 1);
        cat(s, " BAD");
    }
    gfx_text(0, 59, s, C_RED);
    /* Card read speed while loading, and the folder's time to all samples. */
    s[0] = 0;
    if (us) {
        fmt_fixed(s, (int)((uint64_t)samples_read_bytes() * 10u / us), 1, 1, 0);
        cat(s, mmc_use_dma ? " MB/S DMA" : " MB/S");
    }
    if (storage_sample_load_us()) {
        cat(s, " IN ");
        fmt_fixed(s + strlen(s), (int)(storage_sample_load_us() / 10000u), 2, 1, 0);
        cat(s, " S");
    }
    gfx_text(0, 69, s, C_DIM);
    field(0, 82, &f_msel, 0, 0);
    if (samples_count()) {
        const struct sample *m = &samples[get_msel(0)];
        s[0] = 0;
        cat(s, m->err ? m->err : states[m->state]);
        gfx_text(0, 93, s, m->state == SMP_READY ? C_GREEN : m->state >= SMP_NOMEM ? C_RED : C_DIM);
        if (m->state >= SMP_HEADER && m->state <= SMP_NOMEM) {
            char *e = put_uint(s, m->channels, 1);
            cat(e, "CH ");
            e = put_uint(s + strlen(s), m->bits, 1);
            cat(e, "B ");
            put_uint(s + strlen(s), m->rate, 1);
            gfx_text(0, 103, s, C_WHITE);
            e = put_uint(s, m->frames, 1);
            cat(e, " FR");
            gfx_text(0, 113, s, C_WHITE);
        }
        if (m->state == SMP_READY) {
            put_hex(s, m->crc);
            gfx_text_right(127, 113, s, C_DIM);
        }
    }
}

static void draw_sdcard(void)
{
    char s[24];

    gfx_title(0, 2, "SD CARD USAGE", C_TITLE);
    if (!sd_total_kb) {
        gfx_text(0, 24, storage_card() ? "CAN'T READ CARD" : "NO CARD", C_DIM);
        return;
    }
    usage_row(18, "TOTAL", sd_total_kb);
    usage_row(29, "USED", sd_total_kb - sd_free_kb);
    usage_row(40, "FREE", sd_free_kb);
    usage_bar(52, sd_total_kb - sd_free_kb, sd_total_kb);
    gfx_text(0, 70, "FOLDERS", C_DIM);
    put_uint(s, (unsigned)storage_folders(), 1);
    gfx_text_right(127, 70, s, C_WHITE);
}

static void draw_prefs(void)
{
    gfx_title(0, 2, "PREFERENCES", C_TITLE);
    gfx_text(0, 18, "SCREEN BRIGHTNESS", C_DIM);
    field(0, 18, &f_bright, 0, 127);
    gfx_text(0, 30, "SCREEN SAVER", C_DIM);
    field(0, 30, &f_saver, 0, 127);
    gfx_text(0, 42, "GATE DELAY", C_DIM);
    field(0, 42, &f_gate_delay, 0, 127);
    gfx_text(0, 54, "AUTO-AUDITION", C_DIM);
    field(0, 54, &f_pref_aud, 0, 127);
    gfx_text(0, 66, "LOAD FOLDER WARN", C_DIM);
    field(0, 75, &f_pref_warn, 0, 127);
    gfx_text(0, 88, "BROWSE PREVIEW", C_DIM);
    field(0, 88, &f_pref_preview, 0, 127);
    gfx_text(0, 100, "PRESET LOAD", C_DIM);
    field(0, 100, &f_pref_load, 0, 127);
}

/* Stock's Utility > Inc/Dec Preset: per folder, a CV rise to 1.7 V steps
 * (after <= 1.5 V); empty presets are stops. */
static void draw_incdec(void)
{
    static const char *const names[PCV_COUNT] = { "INC PRESET CV", "DEC PRESET CV", "RESET PRESET CV" };
    char s[24] = "";

    gfx_title(0, 2, "INC/DEC PRESET", C_TITLE);
    copy_n(s, storage_folder()[0] ? storage_folder() : "NO FOLDER", 21);
    gfx_text(0, 15, s, C_DIM);
    for (int i = 0; i < PCV_COUNT; i++) {
        gfx_title(0, 28 + i * 24, names[i], C_TITLE);
        field(0, 39 + i * 24, &f_pcv, i, 0);
    }
    if (storage_cv_latency_us) {
        memcpy(s, "LAST CHANGE ", 13);
        put_uint(s + 12, storage_cv_latency_us, 1);
        cat(s, " US");
        gfx_text(0, 112, s, C_FAINT);
    }
}

/* "A8M 2026.4.0-dev+8799c16": head = up to the '+' (at most n chars),
 * returns what follows it ("" for a release or an older build). */
static const char *ver_split(char *head, const char *v, int n)
{
    int i = 0;

    for (; v[i] && v[i] != '+' && i < n; i++) {
        head[i] = v[i];
    }
    head[i] = 0;
    while (v[i] && v[i] != '+') {
        i++;
    }
    return v[i] ? v + i + 1 : "";
}

/* A build part as a short tag beside a label: "8799c16.dirty" -> "+8799c16". */
static void build_tag(int y, const char *b)
{
    char t[12] = "+";
    int i = 0;

    for (; b[i] && b[i] != '.' && i < 9; i++) {
        t[1 + i] = b[i];
    }
    t[i ? 1 + i : 0] = 0;
    gfx_text_right(124, y, t, C_DIM);
}

static void draw_about(void)
{
    gfx_title(0, 2, "ABOUT", C_TITLE);
    gfx_text(0, 18, "ASSIMIL8OR MACHINES", C_WHITE);
    gfx_text(0, 30, "FIRMWARE", C_DIM);
    {
        char s[24];
        ver_split(s, fw_version, 19);
        gfx_text(0, 40, s, C_WHITE);
        gfx_text_right(127, 30, FW_HASH, C_DIM);
    }
    gfx_text(0, 52, "MACHINES", C_DIM);
    {
        char s[32];
        char *e = put_uint(s, VOICE_COUNT - 1, 1);
        cat(e, " VOICES");
        gfx_text(0, 61, s, C_WHITE);
        s[0] = 0;
        for (int f = 0; f < FAMILY_COUNT; f++) {
            if (f) {
                cat(s, " ");
            }
            cat(s, families[f].name);
        }
        gfx_text(0, 70, s, C_WHITE);
    }
    gfx_text(0, 79, "CV CAL", C_DIM);
    gfx_text(42, 79, cal_source, C_WHITE);
    gfx_text(0, 88, "FOLDER", C_DIM);
    {
        char s[24];
        copy_n(s, storage_folder()[0] ? storage_folder() : storage_card() ? "NONE" : "NO CARD", 14);
        gfx_text(42, 88, s, C_WHITE);
        /* Presets loaded, and the folder's complete load time. */
        char *e = put_uint(s, (unsigned)(storage_total() - storage_pending()), 1);
        cat(e, " IN ");
        fmt_fixed(e + 4, (int)(storage_load_us() / 10000u), 2, 1, 0);
        cat(s, " S");
        gfx_text(0, 97, s, C_WHITE);
    }
    if (storage_error) {
        gfx_text(0, 106, storage_error, C_RED);
    }
    {
        char s[12];
        put_uint(s, mpu_mhz(), 1);
        gfx_text(0, 117, "MPU", C_DIM);
        gfx_text(24, 117, s, C_WHITE);
        gfx_text(24 + gfx_text_width(s) + 6, 117, "MHZ", C_DIM);
    }
}

#define MARQUEE_DWELL_US 1000000u
#define MARQUEE_PAUSE_US 1000000u
#define MARQUEE_PX_PER_S 30

/* x offset of a label `over` pixels too wide, highlighted since `since`:
 * still for the dwell, then scrolls to its end, pauses, jumps back. */
static int marquee_off(int over, uint32_t since)
{
    uint32_t t = time_us() - since, run = (uint32_t)over * 1000000u / MARQUEE_PX_PER_S;

    if (t < MARQUEE_DWELL_US) {
        return 0;
    }
    t = (t - MARQUEE_DWELL_US) % (run + 2 * MARQUEE_PAUSE_US);
    return t < run ? (int)((uint64_t)t * MARQUEE_PX_PER_S / 1000000u) : t < run + MARQUEE_PAUSE_US ? over : 0;
}

/* Drops a trailing ".wav" (any case). */
static void strip_wav(char *s)
{
    int n = (int)strlen(s);

    if (n > 4 && s[n - 4] == '.' && (s[n - 3] | 32) == 'w' && (s[n - 2] | 32) == 'a' && (s[n - 1] | 32) == 'v') {
        s[n - 4] = 0;
    }
}

/* One row of a list: selection bar over [bx, bx+bw), the label from tx
 * (the highlighted one scrolls when it doesn't fit, from `since`), a yellow
 * tick on the current item. */
static void list_row(int bx, int bw, int tx, int y, const char *s, uint16_t c, int sel, int cur,
                     uint32_t since)
{
    int x = tx, over = gfx_text_width(s) - 1 - (bx + bw - 2 - tx);

    if (sel) {
        gfx_rect(bx, y - 2, bw, 10, C_SELBAR);
        if (over > 0) {
            x -= marquee_off(over, since);
        }
    }
    oled_clip_x(tx, bx + bw - 2);
    gfx_text(x, y, s, !sel && cur ? C_YELLOW : c);
    oled_clip_x(0, OLED_W);
    if (cur) {
        gfx_rect(bx, y, 2, 7, C_YELLOW);
    }
}

static void draw_folders(const char *title, const struct field *f)
{
    int n = storage_folders(), rows = 10, top = focus[page] - rows / 2;
    char s[64];
    static int last_page = -1, last_focus;
    static uint32_t since;

    if (page != last_page || focus[page] != last_focus) {
        last_page = page;
        last_focus = focus[page];
        since = time_us();
    }

    gfx_title(0, 2, title, C_TITLE);
    if (!n) {
        gfx_text(0, 24, storage_card() ? "NO FOLDERS" : "NO CARD", C_DIM);
        return;
    }
    top = top > n - rows ? n - rows : top;
    top = top < 0 ? 0 : top;
    for (int i = 0; i < n; i++) {
        int foc = field_reg(f, i), y = 18 + (i - top) * 10;
        int cur = !strcmp_(storage_folder_name(i), storage_folder());
        if (i < top || i >= top + rows) {
            continue;
        }
        if (cur && storage_changed_count()) {
            draw_tri(3, y + 1, 4, C_YELLOW);
        }
        copy_n(s, storage_folder_name(i), 62);
        list_row(0, 128, 9, y, s, C_WHITE, foc, cur, since);
    }
}

static void draw_import(void)
{
    int n = imp_rows(), rows = 9, top = focus[P_IMPORT] - rows / 2;
    char s[64], t[8] = "CH";
    int tgt = ieq(imp.dir, storage_folder()) ? imp.snap.ch[imp.ch].s.zone[imp.from == P_ZONES ? imp.z : 0].smp - 1 : -1;

    gfx_title(0, 2, "IMPORT", C_TITLE);
    t[2] = (char)('1' + imp.ch);
    t[3] = 0;
    if (imp.from == P_ZONES) {
        memcpy(t + 3, " Z", 2);
        t[5] = (char)('1' + imp.z);
        t[6] = 0;
    }
    gfx_text_right(127, 3, t, C_ORANGE);
    s[0] = '/';
    if (strlen(imp.dir) > 19) {
        memcpy(s + 1, "..", 2);
        copy_n(s + 3, imp.dir + strlen(imp.dir) - 17, 17);
    } else {
        copy_n(s + 1, imp.dir, 30);
    }
    gfx_text(0, 14, s, C_DIM);
    top = top > n - rows ? n - rows : top;
    top = top < 0 ? 0 : top;
    for (int row = top; row < n && row < top + rows; row++) {
        int y = 26 + (row - top) * 10, e = imp_entry(row), cur = 0;
        uint16_t c = C_WHITE;

        if (e < 0) {
            memcpy(s, "../", 4);
            c = C_DIM;
        } else {
            copy_n(s, storage_bname(e), 62);
            if (storage_bisdir(e)) {
                cat(s, "/");
                c = C_TITLE;
            } else {
                strip_wav(s);
                cur = tgt >= 0 && ieq(storage_bname(e), samples[tgt].name);
            }
        }
        list_row(0, 128, 5, y, s, c, row == focus[P_IMPORT], cur, imp.moved_at);
    }
    if (!n) {
        gfx_text(3, 26, "NO WAVS", C_DIM);
    }
    gfx_text(0, 120, pref_auto_audition ? "AUDITION ON" : "AUDITION OFF", C_FAINT);
}

static void draw_fname(void)
{
    static const char *const titles[] = { "NEW FOLDER", "RENAME FOLDER", "DUPLICATE FOLDER", "RENAME SAMPLE" };
    static const char *const go[] = { "CREATE", "RENAME", "DUPLICATE", "RENAME" };
    const char *label = go[fn_mode];
    int w = gfx_text_width(label) + 6, end = FNAME_LEN;

    gfx_title(0, 2, titles[fn_mode], C_TITLE);
    while (end > 0 && (fname[end - 1] == ' ' || !fname[end - 1])) {
        end--;
    }
    for (int i = 0; i < FNAME_LEN; i++) {
        int x = 3 + (i % FNAME_ROW) * 9, y = 20 + (i / FNAME_ROW) * 12;
        if (i > end || !field(x, y, &f_fname, i, 0)) {
            gfx_rect(x, y + 9, 5, 1, C_FAINT);
        }
    }
    if (field_reg(&f_fname_go, 0)) {
        gfx_text_box(127 - w + 3, 47, label, C_BLACK, C_WHITE);
    } else {
        gfx_frame(127 - w, 45, w, 11, C_DIM);
        gfx_text(127 - w + 3, 47, label, C_WHITE);
    }
    if (fn_mode != FN_NEW) {
        char s[24];
        gfx_text(0, 78, "FROM", C_DIM);
        copy_n(s, fn_src, 19);
        gfx_text(0, 90, s, C_WHITE);
    }
}

static void sw_bar(int y, int pct, uint16_t c)
{
    char s[8];

    gfx_frame(4, y, 120, 12, C_DIM);
    gfx_rect(6, y + 2, 116 * pct / 100, 8, c);
    put_uint(s, (unsigned)pct, 1);
    cat(s, "%");
    gfx_text_right(124, y + 16, s, C_WHITE);
}

static void draw_software(void)
{
    int st = update_state();
    char s[32];

    gfx_title(0, 2, "LOAD SOFTWARE", C_TITLE);
    switch (st) {
    case UPD_SCANNING:
        gfx_text(4, 24, "READING 1:/APP", C_WHITE);
        sw_bar(44, update_percent(), C_DIM);
        return;
    case UPD_COPYING:
    case UPD_VERIFYING:
        gfx_text(4, 24, st == UPD_COPYING ? "WRITING BOOT CARD" : "VERIFYING", C_WHITE);
        ver_split(s, update_found(), 19);
        gfx_text(4, 34, s, C_DIM);
        sw_bar(50, update_percent(), C_ORANGE);
        gfx_text(4, 100, "ANY BUTTON CANCELS", C_DIM);
        return;
    case UPD_DONE:
        if (!sw_done_at) {
            sw_done_at = time_us() | 1u;
        }
        gfx_text(4, 30, "INSTALLED", C_GREEN);
        gfx_text(4, 52, ver_split(s, update_found(), 19), C_DIM);
        gfx_text(4, 42, s, C_WHITE);
        gfx_text(4, 64, "RESTARTING...", C_YELLOW);
        return;
    }
    sw_done_at = 0;
    if (st == UPD_IDLE) {
        gfx_text(4, 24, "INSERT A CARD WITH", C_WHITE);
        gfx_text(4, 34, "THE SOFTWARE (APP)", C_WHITE);
        gfx_text(4, 44, "AT ITS ROOT, THEN", C_WHITE);
        gfx_text(4, 54, "CLICK DATA1", C_WHITE);
        gfx_text(4, 117, "UTILITY: EXIT", C_FAINT);
        return;
    }
    if (st == UPD_NONE) {
        gfx_text(4, 24, update_error ? update_error : "NO APP ON CARD", C_YELLOW);
        gfx_text(4, 38, "PUT A FILE NAMED APP", C_DIM);
        gfx_text(4, 48, "IN THE CARD'S ROOT", C_DIM);
    } else if (st == UPD_BAD || st == UPD_FAILED) {
        gfx_text(4, 18, st == UPD_BAD ? "APP REJECTED" : "INSTALL FAILED", C_RED);
        gfx_text(4, 30, update_error ? update_error : "", C_WHITE);
        if (st == UPD_BAD) {
            put_uint(s, update_size(), 1);
            cat(s, " BYTES");
            gfx_text(4, 40, s, C_DIM);
        }
        gfx_text(4, 52, update_error && !strcmp_(update_error, "CHECK THE BOOT CARD") ?
                 "BOOT CARD STATE UNKNOWN" : "BOOT CARD UNCHANGED", C_DIM);
    } else if (st == UPD_FOUND) {
        gfx_text(4, 16, "ON CARD", C_DIM);
        build_tag(16, ver_split(s, update_found(), 19));
        gfx_text(4, 26, s, update_kind() == APP_UNKNOWN ? C_YELLOW : C_WHITE);
        put_uint(s, update_size(), 1);
        cat(s, " BYTES");
        gfx_text(4, 36, s, C_DIM);
        if (!strcmp_(update_found(), fw_version)) {
            gfx_text(4, 46, "SAME AS RUNNING", C_YELLOW);
        }
    }
    gfx_text(4, 64, "RUNNING", C_DIM);
    build_tag(64, ver_split(s, fw_version, 19));
    gfx_text(4, 74, s, C_WHITE);
    if (st == UPD_FOUND && storage_changed_count()) {
        put_uint(s, (unsigned)storage_changed_count(), 1);
        cat(s, " UNSAVED PRESETS");
        gfx_text(4, 90, s, C_YELLOW);
    }
    gfx_text_right(127, 117, st == UPD_FOUND ? "CLICK: INSTALL" : "CLICK: LOOK AGAIN", C_FAINT);
}

/* ---- CALIBRATE: stock's cal.bin as a scrolling table; edits are live,
 * captures follow stock's Calibrate steps, SAVE writes 0:cal.bin. ---- */

enum {
    CR_ZERO_ALL, CR_GAIN_ALL, CR_HEAD, CR_CV, CR_IN_CAP = CR_CV + CAL_CV, CR_IN, CR_OUT_CAP = CR_IN + 2, CR_OUT,
    CR_MIX = CR_OUT + 8, CR_SAVE = CR_MIX + 2, CR_ROWS
};
#define CAL_VIS_ROWS 10

static int cal_row_fields(int r)
{
    return r == CR_HEAD ? 0 : r >= CR_CV && r < CR_IN_CAP ? 3 : 1;
}

static const char cal_group[] = "ABC";
static const uint16_t cal_group_c[] = { C_RED, C_GREEN, 0x3a7fu };

static void cal_name(char *o, int i)
{
    o[0] = (char)('1' + (i & 7));
    o[1] = cal_group[i / 8];
    o[2] = 0;
}

/* An edit range that also holds the loaded value. */
static void cal_range(int v, int lo, int hi, int *min, int *max)
{
    *min = v < lo ? v : lo;
    *max = v > hi ? v : hi;
}

static int get_cal_cvoff(int i) { return cal_live.cv_off[i]; }
static void set_cal_cvoff(int i, int v) { cal_live.cv_off[i] = (int16_t)v; cal_msg = 0; }
static void range_cal_cvoff(int i, int *lo, int *hi)
{
    cal_range(cal_loaded.cv_off[i], CAL_OFF_NEUTRAL - 2048, CAL_OFF_NEUTRAL + 2048, lo, hi);
}
/* The shift it gives at mid scale (code 2048) at the input's gain. */
static void fmt_cal_cvoff(char *o, int v, int i)
{
    float mv = (float)(v - CAL_OFF_NEUTRAL) * (float)cal_live.cv_gain[i] * (4.0f / 32768.0f) * (10000.0f / 6553.6f);
    int t = (int)(mv + (mv < 0 ? -0.5f : 0.5f));

    if (t > -1000 && t < 1000) {
        fmt_fixed(o, t, 1, 1, 1);
    } else {
        fmt_fixed(o, t / 10, 0, 1, 1);
    }
}
static void hold_cal_cvoff(int i) { cal_live.cv_off[i] = cal_loaded.cv_off[i]; ui_say("LOADED VALUE"); }
static const struct field f_cal_cvoff = { get_cal_cvoff, set_cal_cvoff, range_cal_cvoff, fmt_cal_cvoff, 0, L_NONE, 0,
                                          hold_cal_cvoff };

static int get_cal_gain(int i) { return cal_live.cv_gain[i]; }
static void set_cal_gain(int i, int v) { cal_live.cv_gain[i] = (int16_t)v; cal_msg = 0; }
static void range_cal_gain(int i, int *lo, int *hi) { cal_range(cal_loaded.cv_gain[i], -20480, -15360, lo, hi); }
/* Ratio to stock's default gain. */
static void fmt_cal_gain(char *o, int v, int i)
{
    (void)i;
    fmt_fixed(o, (int)(((int64_t)v * 10000 + (v < 0 ? -8960 : 8960)) / CAL_GAIN_DEFAULT), 4, 1, 0);
}
static void hold_cal_gain(int i) { cal_live.cv_gain[i] = cal_loaded.cv_gain[i]; ui_say("LOADED VALUE"); }
static const struct field f_cal_gain = { get_cal_gain, set_cal_gain, range_cal_gain, fmt_cal_gain, 0, L_NONE, 0,
                                         hold_cal_gain };

/* Sample input and mix offsets in 0.01 mV steps (word full scale 10 V; the
 * mix word gets twice its offset). */
#define WORDS_PER_10UV 2147.483648
static int32_t cal_round(double v)
{
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}
static int get_cal_in(int lr) { return cal_round(cal_live.in_off[lr] / WORDS_PER_10UV); }
static void set_cal_in(int lr, int v) { cal_live.in_off[lr] = cal_round(v * WORDS_PER_10UV); cal_msg = 0; }
static void range_cal_in(int lr, int *lo, int *hi)
{
    cal_range(cal_round(cal_loaded.in_off[lr] / WORDS_PER_10UV), -14648, 14648, lo, hi);
}
static void fmt_cal_mv2(char *o, int v, int a) { (void)a; fmt_fixed(o, v, 2, 1, 1); }
static void hold_cal_in(int lr) { cal_live.in_off[lr] = cal_loaded.in_off[lr]; ui_say("LOADED VALUE"); }
static const struct field f_cal_in = { get_cal_in, set_cal_in, range_cal_in, fmt_cal_mv2, 0, L_NONE, 0, hold_cal_in };

static int get_cal_mix(int lr) { return cal_round(cal_live.mix_off[lr] * 2.0 / WORDS_PER_10UV); }
static void set_cal_mix(int lr, int v) { cal_live.mix_off[lr] = cal_round(v * WORDS_PER_10UV / 2.0); cal_msg = 0; }
static void range_cal_mix(int lr, int *lo, int *hi)
{
    cal_range(cal_round(cal_loaded.mix_off[lr] * 2.0 / WORDS_PER_10UV), -20019, 20019, lo, hi);
}
static void hold_cal_mix(int lr) { cal_live.mix_off[lr] = cal_loaded.mix_off[lr]; ui_say("LOADED VALUE"); }
static const struct field f_cal_mix = { get_cal_mix, set_cal_mix, range_cal_mix, fmt_cal_mv2, 0, L_NONE, 0,
                                        hold_cal_mix };

/* Channel outputs: stock's byte (x 65536 on the word, ~0.305 mV). */
static int get_cal_ch(int n) { return cal_live.ch_off[n]; }
static void set_cal_ch(int n, int v) { cal_live.ch_off[n] = (int8_t)v; cal_msg = 0; }
static void range_cal_ch(int n, int *lo, int *hi) { (void)n; *lo = -127; *hi = 127; }
static void fmt_cal_ch(char *o, int v, int n)
{
    (void)n;
    fmt_fixed(o, (int)(((int64_t)v * 6553600000LL + (v < 0 ? -1073741824LL : 1073741824LL)) / 2147483648LL), 1, 1, 1);
}
static void hold_cal_ch(int n) { cal_live.ch_off[n] = cal_loaded.ch_off[n]; ui_say("LOADED VALUE"); }
static const struct field f_cal_ch = { get_cal_ch, set_cal_ch, range_cal_ch, fmt_cal_ch, 0, L_NONE, 0, hold_cal_ch };

static void act_cal(int r)
{
    static const int cv_items[] = { M_CANCEL, M_CAL_ZERO, M_CAL_GAIN };
    static const int save_items[] = { M_CANCEL, M_CAL_SAVE };

    cal_msg = cal_msg2 = 0;
    if (r == CR_ZERO_ALL || r == CR_GAIN_ALL) {
        cal_capture(r == CR_ZERO_ALL ? CAPK_ZERO : CAPK_GAIN, (1u << CAL_CV) - 1);
    } else if (r == CR_IN_CAP) {
        cal_capture(CAPK_IN, 3);
    } else if (r == CR_OUT_CAP) {
        cal_capture(CAPK_OUT, 0);
    } else if (r == CR_SAVE) {
        copy_n(menu_title, "SAVE CAL.BIN?", 23);
        menu_open(menu_title, save_items, 2);
    } else if (r >= CR_CV && r < CR_IN_CAP) {
        cal_menu_input = r - CR_CV;
        cal_name(menu_title, cal_menu_input);
        cat(menu_title, " CAPTURE");
        menu_open(menu_title, cv_items, 3);
    }
}
static const struct field f_cal_act = { 0, 0, 0, 0, act_cal, L_NONE, 0, 0 };

static void cal_do_save(int leave)
{
    int ok = cal_save();

    ui_say(ok ? "CAL SAVED" : "CAL SAVE FAILED");
    if (ok && leave) {
        open_page(P_UTIL);
    }
}

static void cal_action_row(int y, int r, const char *label)
{
    if (field_reg(&f_cal_act, r)) {
        gfx_rect(0, y - 2, 128, 10, C_SELBAR);
    }
    gfx_text(3, y, label, C_WHITE);
    gfx_text_right(126, y, "...", C_WHITE);
}

static void cal_mv_text(int right, int y, int32_t words, uint16_t c)
{
    char s[12];

    int v = (int)(((int64_t)words * 1000000 + (words < 0 ? -1073741824LL : 1073741824LL)) / 2147483648LL);

    if (v > -100000 && v < 100000) {
        fmt_fixed(s, v, 2, 1, 1);
    } else {
        fmt_fixed(s, v / 100, 0, 1, 1);
    }
    gfx_text_right(right, y, s, c);
}

static void cal_grid_cell(int x, int y, const char *s, int bit, uint32_t mask, uint32_t ok, uint32_t neg)
{
    uint16_t c = !(mask >> bit & 1) ? C_FAINT : ok >> bit & 1 ? C_GREEN : neg >> bit & 1 ? C_TITLE : C_RED;

    gfx_text(x, y, s, c);
}

/* The 24 inputs as stock's grid: green ok, blue -4 V, red not, faint not
 * taking part. */
static void cal_cv_grid(int y, uint32_t ok, uint32_t neg)
{
    uint32_t mask = cal_capture_mask();

    for (int g = 0; g < 3; g++) {
        char n[2] = { cal_group[g], 0 };
        gfx_text(4, y + g * 11, n, cal_group_c[g]);
        for (int k = 0; k < 8; k++) {
            n[0] = (char)('1' + k);
            cal_grid_cell(20 + k * 13, y + g * 11, n, g * 8 + k, mask, ok, neg);
        }
    }
}

static void cal_progress(int y)
{
    int n = cal_capture_progress();

    gfx_text(4, y, "MEASURING...", C_YELLOW);
    gfx_frame(4, y + 12, 120, 10, C_DIM);
    gfx_rect(6, y + 14, 116 * n / CAL_SAMPLES, 6, C_ORANGE);
}

/* One input: its reading in volts. The capture runs on stock's neutral
 * gain (-16384), so the default gain's scale is put back. */
static void cal_single(int y)
{
    uint32_t m = cal_capture_mask();
    char s[16];

    if (m && !(m & (m - 1))) {
        int i = __builtin_ctz(m);
        int64_t c = (int64_t)cal_cv_c(i) * -CAL_GAIN_DEFAULT * 10000;
        int64_t d = (int64_t)-CAL_OFF_NEUTRAL * 65536;
        cal_name(s, i);
        gfx_title(4, y, s, cal_group_c[i / 8]);
        fmt_fixed(s, (int)((c + (c < 0 ? -d / 2 : d / 2)) / d), 3, 1, 1);
        cat(s, " V");
        gfx_text(30, y + 1, "READS", C_DIM);
        gfx_text(66, y + 1, s, C_WHITE);
    }
}

static void cal_result(uint32_t m, uint32_t ok, const char *bad, const char *good)
{
    gfx_text(4, 16, (m & ~ok) ? bad : good, (m & ~ok) ? C_RED : C_GREEN);
    if (m & ~ok) {
        gfx_text(4, 26, "KEPT THEIR VALUES", C_RED);
    }
}

static void draw_cal_capture(void)
{
    int st = cal_capture_state();
    uint32_t ok = cal_capture_ok(), m = cal_capture_mask();

    switch (st) {
    case CAP_ZERO_WAIT: case CAP_ZERO_MEAS: case CAP_ZERO_DONE:
        gfx_title(0, 2, "CAL: 0 V", C_TITLE);
        if (st == CAP_ZERO_WAIT) {
            gfx_text(4, 16, "APPLY 0 V TO THE", C_WHITE);
            gfx_text(4, 26, "INPUTS, THEN CLICK", C_WHITE);
            cal_cv_grid(44, ok, 0);
            gfx_text(4, 82, "GREEN: NEAR 0 V", C_DIM);
            cal_single(96);
        } else if (st == CAP_ZERO_MEAS) {
            cal_progress(30);
        } else {
            cal_result(m, ok, "RED: NOT AT 0 V,", "OFFSETS UPDATED");
            cal_cv_grid(44, ok, 0);
        }
        break;
    case CAP_GAIN1_WAIT: case CAP_GAIN1_MEAS: case CAP_GAIN2_WAIT: case CAP_GAIN2_MEAS: case CAP_GAIN_DONE:
        gfx_title(0, 2, "CAL: +-4 V", C_TITLE);
        if (st == CAP_GAIN1_WAIT) {
            gfx_text(4, 16, "APPLY +4 V OR -4 V,", C_WHITE);
            gfx_text(4, 26, "THEN CLICK", C_WHITE);
            cal_cv_grid(44, ok, cal_capture_neg());
            gfx_text(4, 82, "GREEN: +4, BLUE: -4", C_DIM);
            cal_single(96);
        } else if (st == CAP_GAIN2_WAIT) {
            gfx_text(4, 16, "NOW THE OPPOSITE:", C_WHITE);
            gfx_text(4, 26, "-4 V WHERE +4 V WAS,", C_WHITE);
            gfx_text(4, 36, "+4 V WHERE -4 V WAS", C_WHITE);
            cal_cv_grid(50, ok, 0);
            gfx_text(4, 86, "GREEN: READY", C_DIM);
            cal_single(98);
        } else if (st == CAP_GAIN_DONE) {
            cal_result(m, ok, "RED: NO VALID GAIN,", "GAINS UPDATED");
            cal_cv_grid(44, ok, 0);
        } else {
            gfx_text(4, 16, st == CAP_GAIN1_MEAS ? "STEP 1 OF 2" : "STEP 2 OF 2", C_DIM);
            cal_progress(30);
        }
        break;
    case CAP_OUT_WAIT: case CAP_OUT_MEAS: case CAP_OUT_DONE:
        gfx_title(0, 2, "CAL: OUTPUTS", C_TITLE);
        if (st == CAP_OUT_WAIT) {
            gfx_text(4, 16, "PATCH OUT N TO CV NA,", C_WHITE);
            gfx_text(4, 26, "MIX L/R TO IN L/R,", C_WHITE);
            gfx_text(4, 36, "THEN CLICK", C_WHITE);
        } else if (st == CAP_OUT_MEAS) {
            char s[8] = "PASS ";
            fmt_int(s + 5, cal_out_pass());
            gfx_text(4, 16, s, C_DIM);
            cal_progress(30);
        } else {
            cal_result(m, ok, "RED: NOT CONNECTED,", "OFFSETS UPDATED");
            gfx_text(4, 42, "RESIDUAL MV AS READ", C_DIM);
            for (int n = 0; n < 10; n++) {
                int x = n < 5 ? 4 : 68, y = 54 + (n % 5) * 10;
                char s[2] = { n < 8 ? (char)('1' + n) : n == 8 ? 'L' : 'R', 0 };
                cal_grid_cell(x, y, s, n, m, ok, 0);
                if (ok >> n & 1) {
                    cal_mv_text(x + 56, y, cal_out_resid(n), C_WHITE);
                }
            }
            gfx_text(4, 106, "UNPATCH LOOP CABLES", C_YELLOW);
        }
        if (st == CAP_OUT_WAIT) {
            gfx_text(4, 60, "OUT", C_DIM);
            for (int n = 0; n < 10; n++) {
                char s[2] = { n < 8 ? (char)('1' + n) : n == 8 ? 'L' : 'R', 0 };
                cal_grid_cell(26 + n * 10, 60, s, n, m, ok, 0);
            }
            gfx_text(4, 74, "GREEN: CONNECTED", C_DIM);
            if (rec_monitor_set()) {
                gfx_text(4, 86, "INPUT MONITOR OFF", C_YELLOW);
            }
        }
        if (st != CAP_OUT_DONE) {
            gfx_text(4, 98, "OUTPUTS MUTED", C_YELLOW);
        }
        break;
    case CAP_IN_WAIT: case CAP_IN_MEAS: case CAP_IN_DONE:
        gfx_title(0, 2, "CAL: SAMPLE IN", C_TITLE);
        if (st == CAP_IN_WAIT) {
            gfx_text(4, 16, "UNPLUG IN L AND R", C_WHITE);
            gfx_text(4, 26, "(OR 0 V), THEN CLICK", C_WHITE);
        } else if (st == CAP_IN_MEAS) {
            cal_progress(30);
        } else {
            cal_result(m, ok, "RED: TOO FAR OFF,", "OFFSETS UPDATED");
        }
        if (st != CAP_IN_MEAS) {
            for (int lr = 0; lr < 2; lr++) {
                int y = 50 + lr * 12;
                cal_grid_cell(4, y, lr ? "IN R" : "IN L", lr, m, ok, 0);
                if (st == CAP_IN_WAIT) {
                    cal_mv_text(90, y, cal_in_level(lr), C_WHITE);
                    gfx_text(96, y, "MV", C_DIM);
                }
            }
            if (st == CAP_IN_WAIT && rec_monitor_set()) {
                gfx_text(4, 86, "INPUT MONITOR OFF", C_YELLOW);
            }
        }
        break;
    }
    if (st == CAP_ZERO_MEAS || st == CAP_GAIN1_MEAS || st == CAP_GAIN2_MEAS || st == CAP_OUT_MEAS ||
        st == CAP_IN_MEAS) {
        gfx_text(4, 120, "UTILITY: CANCEL", C_FAINT);
    } else if (st == CAP_ZERO_DONE || st == CAP_GAIN_DONE || st == CAP_OUT_DONE || st == CAP_IN_DONE) {
        gfx_text(4, 120, "CLICK: BACK", C_FAINT);
    } else {
        gfx_text(4, 110, "CLICK: MEASURE", C_FAINT);
        gfx_text(4, 120, "UTILITY: CANCEL", C_FAINT);
    }
}

static void draw_cal(void)
{
    int f = focus[P_CAL], row = 0, top, msg = cal_msg != 0, rows = msg ? CAL_VIS_ROWS - 1 : CAL_VIS_ROWS;
    char s[16];

    if (cal_capture_state() != CAP_NONE) {
        draw_cal_capture();
        return;
    }
    for (int r = 0, n = 0; r < CR_ROWS; r++) {
        if (f >= n && f < n + cal_row_fields(r)) {
            row = r;
        }
        n += cal_row_fields(r);
    }
    top = row - rows / 2;
    top = top > CR_ROWS - rows ? CR_ROWS - rows : top;
    top = top < 0 ? 0 : top;
    gfx_title(0, 2, "CALIBRATE", C_TITLE);
    if (cal_dirty()) {
        gfx_text_right(127, 3, "EDITED", C_YELLOW);
    }
    for (int r = 0; r < CR_ROWS; r++) {
        int y = 15 + (r - top) * 10;
        if (r >= top && r < top + rows) {
            gfx_clip_y(13, 13 + rows * 10);
        } else {
            gfx_clip_y(0, 0);
        }
        if (r == CR_ZERO_ALL) {
            cal_action_row(y, r, "0 V CAPTURE: ALL");
        } else if (r == CR_GAIN_ALL) {
            cal_action_row(y, r, "+-4 V CAPTURE: ALL");
        } else if (r == CR_HEAD) {
            gfx_text(0, y, "CV", C_DIM);
            gfx_text_right(45, y, "MV", C_DIM);
            gfx_text_right(85, y, "GAIN", C_DIM);
            gfx_text_right(127, y, "LIVE V", C_DIM);
        } else if (r < CR_IN_CAP) {
            int i = r - CR_CV, c = cal_cv_c(i);
            cal_name(s, i);
            if (field_reg(&f_cal_act, r)) {
                gfx_text_box(1, y, s, C_BLACK, C_WHITE);
            } else {
                gfx_text(1, y, s, cal_group_c[i / 8]);
            }
            field(0, y, &f_cal_cvoff, i, 45);
            field(0, y, &f_cal_gain, i, 85);
            fmt_fixed(s, (int)(((int64_t)c * 10000 + (c < 0 ? -32768 : 32768)) / 65536), 3, 1, 1);
            gfx_text_right(127, y, s, C_WHITE);
        } else if (r == CR_IN_CAP) {
            cal_action_row(y, r, "SAMPLE IN: 0 V");
        } else if (r < CR_OUT_CAP) {
            int lr = r - CR_IN;
            gfx_text(1, y, lr ? "IN R" : "IN L", C_TITLE);
            field(0, y, &f_cal_in, lr, 79);
            cal_mv_text(127, y, cal_in_level(lr), C_WHITE);
        } else if (r == CR_OUT_CAP) {
            cal_action_row(y, r, "OUTPUTS: LOOPBACK");
        } else if (r < CR_MIX) {
            int n = r - CR_OUT;
            memcpy(s, "OUT ", 5);
            s[4] = (char)('1' + n);
            s[5] = 0;
            gfx_text(1, y, s, C_TITLE);
            field(0, y, &f_cal_ch, n, 79);
            fmt_int(s, cal_live.ch_off[n]);
            gfx_text_right(127, y, s, C_DIM);
        } else if (r < CR_SAVE) {
            int lr = r - CR_MIX;
            gfx_text(1, y, lr ? "MIX R" : "MIX L", C_TITLE);
            field(0, y, &f_cal_mix, lr, 79);
        } else {
            cal_action_row(y, r, "SAVE TO BOOT CARD");
        }
    }
    gfx_clip_y(0, OLED_H);
    if (msg) {
        gfx_text(0, 110, cal_msg, cal_failed ? C_RED : C_GREEN);
        if (cal_msg2) {
            gfx_text(0, 120, cal_msg2, cal_failed ? C_WHITE : C_DIM);
        }
    } else if (rec_monitor_set()) {
        gfx_text(0, 120, "MONITOR MUTED HERE", C_YELLOW);
    } else {
        gfx_text(0, 120, "HOLD DATA1: RESET", C_FAINT);
    }
}

static void draw_progress(void)
{
    int st = storage_job_state(), pct = storage_job_percent();
    char s[24];

    gfx_title(0, 2, st == JOB_DELETING ? "DELETING" : st == JOB_CLEANING ? "REMOVING COPY" :
              st == JOB_IMPORTING ? "IMPORTING" : st == JOB_TRUNCATING ? "TRUNCATING" : "DUPLICATING", C_TITLE);
    copy_n(s, storage_job_name(), 19);
    gfx_text(4, 24, s, C_WHITE);
    if (st != JOB_DELETING) {
        int active = st == JOB_COPYING || st == JOB_IMPORTING || st == JOB_TRUNCATING;

        gfx_frame(4, 44, 120, 12, C_DIM);
        gfx_rect(6, 46, 116 * (active ? pct : 100) / 100, 8, active ? C_ORANGE : C_DIM);
        put_uint(s, (unsigned)pct, 1);
        cat(s, "%");
        gfx_text_right(124, 62, s, C_WHITE);
    }
    if (st == JOB_COPYING || st == JOB_IMPORTING || st == JOB_TRUNCATING) {
        gfx_text(4, 100, "ANY BUTTON CANCELS", C_DIM);
    }
}

/* SAVE TO targets around save_slot: the folder's presets and New. */
static void draw_save_targets(int y0, int rows)
{
    int list[16], prev[8], np = 0, n = 0;

    for (int c = save_slot - 1; c >= 1 && np < rows / 2; c--) {
        if (save_candidate(c)) {
            prev[np++] = c;
        }
    }
    while (np) {
        list[n++] = prev[--np];
    }
    list[n++] = save_slot;
    for (int c = save_slot + 1; c <= PRESET_SLOTS && n < rows; c++) {
        if (save_candidate(c)) {
            list[n++] = c;
        }
    }
    for (int i = 0; i < n; i++) {
        int c = list[i], y = y0 + i * 10;
        char s[8];
        if (c == save_slot) {
            gfx_rect(0, y - 2, 128, 10, C_SELBAR);
        }
        put_uint(s, (unsigned)c, 3);
        gfx_text(3, y, s, c == preset_number ? C_YELLOW : C_DIM);
        if (storage_changed(c)) {
            draw_tri(23, y + 1, 3, C_YELLOW);
        }
        gfx_text(27, y, c == save_new ? "NEW" : slot_label(c), c == save_new ? C_DIM : C_WHITE);
    }
}

static void draw_save(void)
{
    int foc;

    gfx_title(0, 2, "SAVE TO", C_TITLE);
    gfx_text(0, 16, "SLOT", C_DIM);
    field(30, 16, &f_save_slot, 0, 0);
    gfx_text(0, 28, "NAME", C_DIM);
    for (int i = 0; i < PRESET_NAME_LEN; i++) {
        int x = 30 + i * 8;
        if (!field(x, 28, &f_name, i, 0)) {
            gfx_rect(x, 37, 5, 1, C_FAINT);
        }
    }
    foc = field_reg(&f_save_go, 0);
    {
        const char *label = save_armed ? "OVERWRITE? CLICK" : "SAVE";
        int w = gfx_text_width(label) + 6;
        if (foc) {
            gfx_text_box(127 - w + 3, 42, label, C_BLACK, C_WHITE);
        } else {
            gfx_frame(127 - w, 40, w, 11, C_DIM);
            gfx_text(127 - w + 3, 42, label, C_WHITE);
        }
    }
    draw_save_targets(58, 7);
}

/* s holds at least 64 bytes. */
static void pop_label(char *s, int v)
{
    const struct field *f = pop.ref.f;

    if (f->list == L_REVERB || f->list == L_RVPROG) {
        rv_label(s, v);
    } else if (f->list == L_MACHINE) {
        s[0] = 0;
        cat(s, v == -3 ? "../" : v == POP_OFF ? "OFF" : pop.mlevel == 1 ? (v ? families[v - 1].name : "SAMPLE") : machine_name(v));
    } else if (f->list == L_SAMPLE && v >= 0 && samples_valid(SMP_INDEX(v))) {
        copy_n(s, samples[SMP_INDEX(v)].name, 62);
        strip_wav(s);
        cat_side(s, v);
    } else if (f->labels) {
        s[0] = 0;
        cat(s, f->labels[v]);
    } else {
        f->fmt(s, v, pop.ref.arg);
    }
}

static void draw_pick_full(void)
{
    char s[64], t[8] = "CH";
    int rows = 10, top = pop.idx - rows / 2, sample = pop.ref.f->list == L_SAMPLE, rv = pop.ref.f->list == L_REVERB || pop.ref.f->list == L_RVPROG;
    int ch = pop.ref.arg / (sample ? ZONES : 1);

    gfx_rect(0, 0, 128, 128, C_BLACK);
    gfx_title(0, 2,
              sample ? "SAMPLE" : pop.ref.f->list == L_RVPROG ? "PROGRAM" : rv ? (pop.ref.f == &f_rvunit ? "REVERB UNIT" : rv_units[rv_unit_now()].name) :
              pop.mlevel == 1 ? "VOICE" : families[pop.mfam].name,
              !sample && pop.mlevel == 1 ? C_DIM : C_TITLE);
    t[2] = (char)('1' + ch);
    t[3] = 0;
    if (pop.ref.f == &f_zsmp) {
        memcpy(t + 3, " Z", 2);
        t[5] = (char)('1' + pop.ref.arg % ZONES);
        t[6] = 0;
    }
    if (!rv) {
        gfx_text_right(127, 3, t, C_ORANGE);
    }
    top = top > pop.n - rows ? pop.n - rows : top;
    top = top < 0 ? 0 : top;
    for (int i = top; i < pop.n && i < top + rows; i++) {
        int v = pop_v(i);

        pop_label(s, v);
        list_row(0, 128, 5, 18 + (i - top) * 10, s, v == -3 ? C_DIM : C_WHITE, i == pop.idx, v == pop.orig, pop.moved_at);
    }
    if (sample || (!rv && pop.mlevel == 2)) {
        gfx_text(0, 120, pref_auto_audition ? "AUDITION ON" : "AUDITION OFF", C_FAINT);
    }
}

static void draw_popup_list(void)
{
    char s[64];
    int rows = pop.n < 10 ? pop.n : 10, w = 56, h, x0, y0, top;

    top = pop.idx - rows / 2;
    top = top < 0 ? 0 : top > pop.n - rows ? pop.n - rows : top;
    if (pop.ref.f->list == L_PICK) {
        w = 116;
    }
    for (int i = top; i < top + rows; i++) {
        pop_label(s, pop_v(i));
        if (gfx_text_width(s) + 12 > w) {
            w = gfx_text_width(s) + 12;
        }
    }
    if (pop.title && gfx_text_width(pop.title) + 12 > w) {
        w = gfx_text_width(pop.title) + 12;
    }
    w = w > 128 ? 128 : w;
    h = rows * 10 + 6 + (pop.title ? 11 : 0);
    x0 = 64 - w / 2;
    y0 = 64 - h / 2;
    gfx_rect(x0, y0, w, h, C_BLACK);
    gfx_frame(x0, y0, w, h, C_TITLE);
    if (pop.title) {
        gfx_text(x0 + 5, y0 + 4, pop.title, C_TITLE);
        y0 += 11;
    }
    for (int i = top; i < top + rows; i++) {
        int v = pop_v(i);

        pop_label(s, v);
        list_row(x0 + 2, w - 4, x0 + 5, y0 + 4 + (i - top) * 10, s, C_WHITE, i == pop.idx, v == pop.orig,
                 pop.moved_at);
    }
}

static void draw_popup(void)
{
    char s[24];
    int list = pop.ref.f->list, cell_w;

    if (list == L_SAMPLE || list == L_MACHINE || list == L_REVERB || list == L_RVPROG) {
        draw_pick_full();
        return;
    }
    if (list != L_CVSRC && list != L_D2CV) {
        draw_popup_list();
        return;
    }
    gfx_rect(0, 0, 128, 128, C_BLACK);
    gfx_frame(0, 0, 128, 128, C_TITLE);
    gfx_text(4, 3, list == L_CVSRC || pop.ref.f == &f_mxbsrc || pop.ref.f == &f_rvsrc ? "CV SOURCE" : "D2CV INPUT", C_TITLE);
    gfx_rect(4, 25, 120, 1, C_FAINT);
    for (int i = 0; i < pop.n; i++) {
        const struct pop_item *it = &pop.item[i];
        cell_w = it->y < 25 ? 30 : 34;
        if (i == pop.idx) {
            gfx_rect(it->x - 2, it->y - 1, cell_w, 9, C_SELBAR);
        }
        pop.ref.f->fmt(s, it->v, pop.ref.arg);
        gfx_text(it->x, it->y, s, i != pop.idx && it->v == pop.orig ? C_YELLOW : C_WHITE);
    }
}

void ui_draw(void)
{
    static uint32_t last_loop;
    uint32_t now = time_us();

    if (last_loop && now - last_loop > loop_us_max_seen) {
        loop_us_max_seen = now - last_loop;
    }
    last_loop = now;
    gfx_clear();
    nfields = 0;
    switch (page) {
    case P_HOME: draw_home(); break;
    case P_CHANNELS: draw_channels(); break;
    case P_MACHINE: draw_machine(); break;
    case P_MACHINE_CV: draw_machine_cv(); break;
    case P_SPOS: draw_pos(0); break;
    case P_LPOS: draw_pos(1); break;
    case P_ZONES: draw_zones(); break;
    case P_PITCH:
        if (is_sampler(sel)) {
            draw_pitch_s();
            break;
        }
        draw_mpage("PITCH", TAG_PITCH, own_pitch);
        break;
    case P_LEVEL:
        if (is_sampler(sel)) {
            draw_level_s();
            break;
        }
        draw_mpage("LEVEL", TAG_LEVEL, own_level);
        break;
    case P_PANMIX:
        if (is_sampler(sel)) {
            draw_panmix_s();
            break;
        }
        draw_header("PAN/MIX");
        draw_channel_column();
        param_block(18, D_PAN, "PAN");
        if (sr_note(54)) {
            break;
        }
        param_block(54, D_MIX, "MIX LEVEL");
        field(0, 65, &f_fader, 0, 127);
        gfx_title(12, 92, "IN MIX", C_TITLE);
        field(70, 92, &f_mix_on, sel, 0);
        break;
    case P_MUTATE:
        if (is_sampler(sel)) {
            draw_mutate_s();
            break;
        }
        draw_mpage("MUTATE", TAG_TIMBRE, own_mutate);
        break;
    case P_ENVELOPE: is_sampler(sel) ? draw_env_s() : (void)draw_mpage("ENVELOPE", TAG_ENV, own_env); break;
    case P_EQ: draw_eq(); break;
    case P_EQCV: draw_eqcv(); break;
    case P_FILTER: draw_filter(); break;
    case P_COMP: draw_comp(); break;
    case P_SENDS: draw_sends(); break;
    case P_REVERB: draw_reverb(); break;
    case P_DELAY: draw_delay(); break;
    case P_MIXCOMP: draw_mixcomp(); break;
    case P_GRAIN: draw_grain(); break;
    case P_GRAINCV: draw_graincv(); break;
    case P_TLOCK: draw_tlock(); break;
    case P_PHASE:
        if (is_sampler(sel)) {
            draw_phase_s();
            break;
        }
        draw_mod_page();
        break;
    case P_SAMPLER_ONLY: draw_sampler_only(); break;
    case P_UTIL: draw_util(); break;
    case P_CVMON: draw_cvmon(); break;
    case P_PANEL: draw_panel(); break;
    case P_AUDIO: draw_audio(); break;
    case P_BENCH: draw_bench(); break;
    case P_PREFS: draw_prefs(); break;
    case P_MEMORY: draw_memory(); break;
    case P_SDCARD: draw_sdcard(); break;
    case P_INCDEC: draw_incdec(); break;
    case P_ABOUT: draw_about(); break;
    case P_LOAD: draw_folders("LOAD FOLDER", &f_folder); break;
    case P_FUTIL: draw_folders("FOLDER UTILITIES", &f_futil_folder); break;
    case P_FNAME: draw_fname(); break;
    case P_PROGRESS: draw_progress(); break;
    case P_SOFTWARE: draw_software(); break;
    case P_CAL: draw_cal(); break;
    case P_IMPORT: draw_import(); break;
    case P_SAVE: draw_save(); break;
    case P_RSETUP: draw_rsetup(); break;
    }
    drawn_page = page;
    draw_rec_notice();
    if (notice[0] && notice_until > time_us64()) {
        gfx_rect(0, 0, 128, 11, C_BLACK);
        gfx_text(1, 1, notice, C_YELLOW);
    }
    if (pop.open) {
        draw_popup();
    }
    {
        int pc = preset_param_channel(sel);
        leds_modes(preset.ch[pc].s.play_mode,
                   is_sampler(pc) ? preset.ch[pc].s.loop_mode : LOOP_OFF);
    }
    {
        static int nomem_seen;
        if (samples_nomem() > nomem_seen) {
            ui_say("NOT ENOUGH MEMORY");
        }
        nomem_seen = samples_nomem();
    }
    draw_toast();
    {
        int l = storage_sample_loading();

        if (l >= 0 && samples[l].frames) {
            gfx_rect(0, 0, (int)((uint64_t)samples[l].loaded * 128 / samples[l].frames), 1, C_GREEN);
        }
    }
    oled_flush();
}

#ifdef COPY_TEST
/* emu/copytest.py: COPY CHANNEL from the live (unsaved) preset carries the
 * channel's EQ, filter (its own and a zone's), Grain, Time Lock, compressor
 * and sends, survives a save/load, and lists the sample by name. */
static void ct_put(const char *s)
{
    while (*s) {
        REG(0x44e09000u) = (uint32_t)*s++;
    }
}

static int ct_fail;

static void ct_check(const char *what, int ok)
{
    ct_put(ok ? "ok   " : "FAIL ");
    ct_put(what);
    ct_put("\n");
    ct_fail |= !ok;
}

static int ct_fx_same(const struct channel_cfg *a, const struct channel_cfg *b)
{
    return !memcmp(&a->eq, &b->eq, sizeof(a->eq)) && !memcmp(&a->filter, &b->filter, sizeof(a->filter)) &&
           !memcmp(&a->grain, &b->grain, sizeof(a->grain)) && !memcmp(&a->tl, &b->tl, sizeof(a->tl)) &&
           !memcmp(&a->s.zone[0].eq, &b->s.zone[0].eq, sizeof(a->s.zone[0].eq)) &&
           !memcmp(&a->s.zone[0].filter, &b->s.zone[0].filter, sizeof(a->s.zone[0].filter)) &&
           a->s.zone[0].beats == b->s.zone[0].beats;
}

static int ct_mix_same(const struct mix_settings *m, int a, int b)
{
    struct mix_channel x, y;

    mix_channel_get(m, (u32)a, &x);
    mix_channel_get(m, (u32)b, &y);
    return !memcmp(&x, &y, sizeof(x));
}

void ui_copy_selftest(void)
{
    static char yml[65536];
    static struct preset back;
    struct channel_cfg *c = &preset.ch[2];
    struct mix_settings *m;
    char label[64];
    int si, n;

    samples_reset("COPYTEST", 0);
    si = samples_add("TEST LOOP.WAV", 0);
    c->machine = 0;
    c->s.zone[0].smp = (uint16_t)(si + 1);
    c->s.zone[0].beats = 3;
    c->eq.on = 1;
    c->eq.hpf = 80;
    c->eq.gain[1] = -60;
    c->eq.frequency[2] = 900;
    c->eq.q[0] = 12;
    c->eq.source[1] = -2;
    c->eq.amount[1] = 40;
    c->filter.type = 2;
    c->filter.cutoff = 700;
    c->filter.resonance = 55;
    c->filter.source[0] = 4;
    c->filter.amount[0] = -30;
    c->s.zone[0].eq.override = 1;
    c->s.zone[0].eq.gain[0] = 25;
    c->s.zone[0].filter.override = 1;
    c->s.zone[0].filter.type = 1;
    c->s.zone[0].filter.cutoff = 500;
    c->grain.value[0] = 1;
    c->grain.value[1] = 250;
    c->grain.value[5] = 40;
    c->grain.source[2] = -3;
    c->grain.amount[2] = 60;
    c->tl.on = 1;
    c->tl.tempo = TIME_LOCK_TEMPO_SPEED;
    c->tl.speed = 150;
    c->tl.input = 7;
    c->tl.ppqn = 2;
    m = mix_edit();
    m->comp[2] = (struct mix_comp_set){ 5, 30, 2, 4, 20, 7, 80, 1 };
    m->reverb_send[2] = 33;
    m->delay_send[2] = 44;
    m->send_pre[2] = 1;
    m->send_route[2][0] = (struct mix_route){ -1, 70 };

    copy_src = preset_number;
    copy_chan_label(label, 2);
    ct_check("list shows the unsaved channel's sample", !memcmp(label, "3 TEST LOOP.WAV", 16));
    copy_chan_label(label, 3);
    ct_check("list shows OFF for an empty channel", !memcmp(label, "4 OFF", 6));

    copy_dst = 5;
    copy_chan_done(2);
    ct_check("channel (EQ, filter, zone EQ/filter, Grain, Time Lock)", !memcmp(&preset.ch[5], c, sizeof(*c)));
    ct_check("compressor, sends, send point, send routes", ct_mix_same(mix_now(), 5, 2));

    copy_dst = 0;
    preset.ch[2].mode = MODE_STEREO;
    copy_chan_done(2);
    ct_check("to channel 1: fx carried, mode Master",
             ct_fx_same(&preset.ch[0], c) && preset.ch[0].mode == MODE_MASTER && ct_mix_same(mix_now(), 0, 2));
    preset.ch[2].mode = MODE_MASTER;

    n = yml_write(&preset, 0, 1, yml, sizeof(yml));
    ct_check("saved and read back", n > 0 && yml_parse(yml, (size_t)n, &back, 0) == YML_OK &&
                                        ct_fx_same(&back.ch[5], c) && ct_mix_same(mix_of(back.mixfx), 5, 2));
    ct_put(ct_fail ? "COPY FAIL\nDONE\n" : "COPY PASS\nDONE\n");
    for (;;) {
    }
}
#endif
