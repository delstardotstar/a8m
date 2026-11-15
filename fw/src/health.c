#include "health.h"
#include "audio.h"
#include "cv.h"
#include "diag.h"
#include "engine.h"
#include "grains.h"
#include "mix_fx.h"
#include "panel.h"
#include "preset.h"
#include "prof.h"
#include "r70.h"
#include "r80.h"
#include "samples.h"
#include "storage.h"
#include "systime.h"
#include "ui.h"
#include "update.h"
#include "version.h"
#include "voices.h"
#include "wdt.h"

#define AUTO_GAP_US 5000000u

static char line[3072];
static unsigned len;
static uint32_t lines, skipped, overloads_seen;
static uint64_t up_us, last_auto_us;
static int want_manual, auto_armed, auto_due, auto_failed, wdt_due;

uint32_t health_lines(void)
{
    return lines;
}

void health_request(void)
{
    want_manual = 1;
}

void health_watchdog(void)
{
    wdt_due = 1;
}

static void s(const char *t)
{
    while (*t && len < sizeof(line) - 2) {
        line[len++] = *t++;
    }
}

static void str(const char *t)
{
    s("\"");
    for (; *t && len < sizeof(line) - 4; t++) {
        unsigned char c = (unsigned char)*t;
        if (c == '"' || c == '\\') {
            line[len++] = '\\';
            line[len++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            line[len++] = (char)c;
        }
    }
    s("\"");
}

static void num(uint64_t v)
{
    char d[24];
    int n = 0;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n && len < sizeof(line) - 2) {
        line[len++] = d[--n];
    }
}

static void key(const char *k)
{
    if (len && line[len - 1] != '{' && line[len - 1] != '[') {
        s(",");
    }
    str(k);
    s(":");
}

static void kint(const char *k, uint64_t v)
{
    key(k);
    num(v);
}

static void kstr(const char *k, const char *v)
{
    key(k);
    str(v);
}

/* Tenths as a decimal. */
static void tenths(uint32_t v)
{
    num(v / 10u);
    s(".");
    num(v % 10u);
}

static void bits8(const char *k, uint8_t b)
{
    char t[9];

    for (int i = 0; i < 8; i++) {
        t[i] = (char)('0' + ((b >> i) & 1));
    }
    t[8] = 0;
    kstr(k, t);
}

static const char *voice_of(const struct channel_cfg *c)
{
    if (c->machine > 0 && c->machine < VOICE_COUNT && voices[c->machine]) {
        return voices[c->machine]->name;
    }
    return c->s.zone[0].smp ? "SAMPLE" : "OFF";
}

static void channels(void)
{
    static const char *const modes[] = { "master", "link", "stereo", "cycle" };
    const struct mix_settings *m = mix_now();

    key("channels");
    s("[");
    for (int ch = 0; ch < CHANNELS; ch++) {
        const struct channel_cfg *c = &preset.ch[ch];
        int smp = c->s.zone[0].smp;
        int zones = 0;

        for (int z = 0; z < ZONES; z++) {
            zones += c->s.zone[z].smp != 0;
        }
        s(ch ? ",{" : "{");
        kstr("voice", voice_of(c));
        if (!c->machine && smp) {
            kstr("sample", samples_valid(smp - 1) ? samples[smp - 1].name : "?");
            kint("zones", (uint64_t)zones);
        }
        kstr("mode", c->mode < MODE_COUNT ? modes[c->mode] : "?");
        key("fx");
        s("[");
        int first = 1;
#define FX(on, name) do { if (on) { s(first ? "" : ","); str(name); first = 0; } } while (0)
        FX(c->eq.on, "eq");
        FX(c->filter.type, "filter");
        FX(m->comp[ch].ratio || m->comp[ch].drive, "comp");
        FX(!c->machine && c->grain.value[GRAIN_ON], "grain");
        FX(!c->machine && c->tl.on, "timelock");
        FX(m->reverb_send[ch], "reverb_send");
        FX(m->delay_send[ch], "delay_send");
#undef FX
        s("]}");
    }
    s("]");
}

static void reverb(void)
{
    const struct mix_settings *m = mix_now();
    const char *name = "";

    if (preset.reverb_unit == REVERB_80) {
        const struct r80_program *p = r80_find(preset.reverb_program);
        name = p ? p->name : "?";
    } else if (preset.reverb_unit == REVERB_70) {
        const struct r70_program *p = r70_find(preset.reverb_program);
        name = p ? p->name : "?";
    } else {
        name = m->reverb_algo < MIX_ALGOS ? mix_algo_names[m->reverb_algo] : "?";
    }
    key("reverb");
    s("{");
    kstr("unit", preset.reverb_unit == REVERB_80 ? "80" : preset.reverb_unit == REVERB_70 ? "70" : "DEP-5");
    kint("program", preset.reverb_program);
    kstr("name", name);
    kint("level", m->reverb_level);
    s("}");
    key("delay");
    s("{");
    kint("level", m->delay_level);
    kint("feedback", m->delay_feedback);
    s("}");
    key("mix_comp");
    s("{");
    kint("ratio", m->master.ratio);
    kint("drive", m->master.drive);
    s("}");
}

static void stages(void)
{
    struct prof_view v;

    prof_get(&v);
    key("stages_us");
    s("{");
    /* RST is the sum of its parts (R-ETC .. R-M1), as on Audio Health. */
    for (int k = 0; k <= PS_COUNT + 1; k++) {
        int i = k < PS_REST ? k : k == PS_REST ? PS_RST_ALL : k <= PS_COUNT ? k - 1 : PS_ALL;
        key(i == PS_RST_ALL ? "RST" : i == PS_ALL ? "ALL" : prof_names[i]);
        s("[");
        num(v.max_us[i]);
        s(",");
        tenths(v.mean_us10[i]);
        s(",");
        num(v.peak_us[i]);
        s("]");
    }
    s("}");
    kint("stage_windows", v.windows);
}

static void hex(const char *k, uint32_t v)
{
    char t[11] = "0x";

    for (int i = 0; i < 8; i++) {
        t[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15u];
    }
    t[10] = 0;
    kstr(k, t);
}

static void hang(void)
{
    const struct wdt_hang *h = wdt_last_hang();

    key("hang");
    if (!h) {
        s("null");
        return;
    }
    s("{");
    hex("pc", h->pc);
    hex("ip", h->ip);
    hex("lr", h->lr);
    hex("sp", h->sp);
    hex("main_tag", h->main_tag);
    hex("irq_tag", h->irq_tag);
    kint("blocks", h->blocks);
    kint("uptime_ms", h->uptime_ms);
    s("}");
}

static void build(const char *trigger)
{
    len = 0;
    s("{");
    kstr("build", fw_version);
    kstr("hash", FW_HASH);
    kstr("folder", storage_folder());
    kint("preset", (uint64_t)preset_number);
    kstr("name", preset.name);
    kint("uptime_ms", up_us / 1000u);
    kstr("trigger", trigger);
    kint("skipped", skipped);
    hex("reset_cause", wdt_reset_cause);
    kint("hang_resets", pref_hang_resets);
    if (trigger[0] == 'w') {
        hang();
    }
    kint("blocks", audio_blocks());
    kint("mix_blocks", audio_stats.m1_blocks);
    kint("late", audio_stats.late_max);
    kint("rep", audio_stats.repeats);
    kint("under", audio_stats.underruns);
    kint("miss", audio_stats.missed);
    kint("isr_us", audio_stats.isr_us_max);
    kint("slow", diag_slow_loops);
    kint("panel_rej", panel_rejects);
    kint("time_us", time_us());
    kint("tcrr", time_ticks());
    kint("render_us", engine_status.render_us_last);
    kint("max_us", engine_status.render_us_max);
    kint("cpu", engine_status.render_us_last * 100u / 667u);
    kint("overld", engine_status.overloads);
    kint("loop_ms", ui_loop_ms_max());
    kint("in_late", audio_stats.in_late);
    kint("in_lost", audio_stats.in_lost);
    kint("m1_rst", audio_stats.m1_restarts);
    kint("m1_fail", audio_stats.m1_restart_fail);
    kint("cv_resync", cv_resyncs);
    kint("cv_lag", cv_read_lag);
    kint("cv_scan_milli", (uint64_t)(cv_scan_rate * 1000.0f + 0.5f));
    key("cv_dev");
    if (cv_dev_min < 0) {
        s("-");
        num((uint64_t)-(int64_t)cv_dev_min);
    } else {
        num((uint64_t)cv_dev_min);
    }
    bits8("awake", engine_status.awake);
    bits8("latched", engine_status.latched);
    stages();
    channels();
    reverb();
    s("}\n");
}

static int write_line(const char *trigger, int manual)
{
    int r;

    build(trigger);
    r = storage_append(HEALTH_PATH, line, len);
    if (r > 0) {
        lines++;
        skipped = 0;
        auto_failed = 0;
        if (manual || trigger[0] == 'o') {
            ui_say(manual ? "HEALTH LOGGED" : "OVERLOAD LOGGED");
        }
    } else {
        if (manual || !auto_failed) {
            skipped++;
        }
        auto_failed |= !manual;
        if (manual) {
            ui_say(r < 0 ? "LOG SKIPPED: BUSY" : storage_error ? storage_error : "LOG FAILED");
        }
    }
    return r > 0;
}

void health_poll(void)
{
    static uint32_t last;
    uint32_t now = time_us(), ov = engine_status.overloads;

    if (last) {
        up_us += now - last;
    }
    last = now;
    /* One line per burst, at most every AUTO_GAP_US; an overload inside
     * the gap or a failed write (busy, no card) is retried, not dropped. */
    if (ov != overloads_seen) {
        overloads_seen = ov;
        auto_due = 1;
    }
    if (auto_due && (!auto_armed || up_us - last_auto_us >= AUTO_GAP_US)) {
        auto_armed = 1;
        last_auto_us = up_us;
        auto_due = !write_line("overload", 0);
    }
    if (wdt_due && (!auto_armed || up_us - last_auto_us >= AUTO_GAP_US)) {
        auto_armed = 1;
        last_auto_us = up_us;
        wdt_due = !write_line("watchdog", 0);
    }
    if (want_manual) {
        want_manual = 0;
        write_line("manual", 1);
    }
}
