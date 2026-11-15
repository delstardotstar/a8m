/*
 * Host test for src/yml.c; built and run by tools/ymltest.sh.
 * usage: ymltest PRSDIR YMLFILE...
 *   1. each YMLFILE (and a CRLF copy, and a generated full 8x8 stock preset):
 *      parse, write, parse again; semantic compare of the stock file with
 *      what we wrote
 *   2. each PRSDIR/P*.PRS and random presets: struct -> yml -> struct equal
 *   3. truncated, bit-flipped and random inputs, tiny output buffers
 *   4. zone sample names (yml_samples) and WAV headers (src/wav.c)
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "preset.h"
#include "eq.h"
#include "grains.h"
#include "r70.h"
#include "r80.h"
#include "timelock.h"
#include "voices.h"
#include "wav.h"
#include "ops.h"
#include "rec.h"
#include "yml.h"

static int failures;

#define CHECK(c, ...) \
    do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct { struct yml_keep k; uint8_t pool[YML_KEEP_FULL]; } kbuf;
#define KEEP (&kbuf.k)

static void kinit(void)
{
    yml_keep_init(KEEP, YML_KEEP_FULL);
}

/* ---- semantic normalisation (independent of yml.c) ---- */

#define MAXL 4096

static int cmpl(const void *a, const void *b) { return strcmp(a, b); }

static int norm(const char *buf, size_t len, char out[][160])
{
    int n = 0;
    char sec[16] = "P";
    size_t pos = 0;

    while (pos < len) {
        char ln[400];
        size_t e = pos, m;
        char *p, *q;

        while (e < len && buf[e] != '\n') e++;
        m = e - pos < sizeof(ln) - 1 ? e - pos : sizeof(ln) - 1;
        memcpy(ln, buf + pos, m);
        ln[m] = 0;
        pos = e + 1;
        p = ln;
        while (*p == ' ' || *p == '\t') p++;
        q = p + strlen(p);
        while (q > p && (q[-1] == ' ' || q[-1] == '\r' || q[-1] == '\t')) *--q = 0;
        if (!*p) continue;
        {
            int num;
            char w[16];
            if (sscanf(p, "%15[A-Za-z] %d :", w, &num) == 2 && p[strlen(p) - 1] == ':') {
                if (!strcasecmp(w, "preset")) strcpy(sec, "P");
                else if (!strcasecmp(w, "channel")) {
                    snprintf(sec, sizeof(sec), "C%d", num);
                } else {
                    char c[8];
                    strncpy(c, sec, sizeof(c)); c[7] = 0;
                    if (strchr(c, 'Z')) *strchr(c, 'Z') = 0;
                    snprintf(sec, sizeof(sec), "%sZ%d", c, num);
                }
                if (strcasecmp(w, "preset")) {
                    snprintf(out[n++], 160, "%s|@", sec);
                } else {
                    snprintf(out[n++], 160, "P|@");
                }
                continue;
            }
        }
        {
            char key[40], val[160], norm[160];
            char *c = strchr(p, ':');
            int i;
            char *tok, *save = 0;
            if (!c) { snprintf(out[n++], 160, "%s|?%s", sec, p); continue; }
            snprintf(key, sizeof(key), "%.*s", (int)(c - p), p);
            for (q = key + strlen(key); q > key && q[-1] == ' '; ) *--q = 0;
            for (i = 0; key[i]; i++) if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 32;
            c++;
            while (*c == ' ') c++;
            snprintf(val, sizeof(val), "%s", c);
            norm[0] = 0;
            for (tok = strtok_r(val, " \t", &save); tok; tok = strtok_r(0, " \t", &save)) {
                char *end;
                double d = strtod(tok, &end);
                char t[64];
                if (*end == 0 && end != tok) snprintf(t, sizeof(t), "#%ld", lround(d * 10000));
                else snprintf(t, sizeof(t), "%s", tok);
                if (norm[0]) strcat(norm, " ");
                strncat(norm, t, sizeof(norm) - strlen(norm) - 1);
            }
            if (!strcmp(key, "zonescv") && strchr(norm, ' ')) *strchr(norm, ' ') = 0;
            {
                static const struct { const char *k; const char *v; } def[] = {
                    { "pitch", "#0" }, { "level", "#0" }, { "pan", "#0" }, { "bits", "#320000" },
                    { "mixlevel", "#0" }, { "channelmode", "#0" }, { "data2ascv", "off" },
                    { "attack", "#0" }, { "release", "#1000" }, { "attackfromcurrent", "#0" },
                    { "aliasing", "#0" }, { "playmode", "#0" }, { "pmsource", "#0" }, { "pmindex", "#0" },
                    { "loopmode", "#0" }, { "looplengthisend", "#0" }, { "reverse", "#0" },
                    { "linamisextenv", "#0" }, { "splicesmoothing", "#0" }, { "zonesrt", "#0" },
                    { "mixmodisfader", "#0" }, { "samplestart", "#0" }, { "sampleend", "#21474836470000" },
                    { "loopstart", "#0" }, { "looplength", "#21474836470000" }, { "pitchoffset", "#0" },
                    { "leveloffset", "#0" }, { "minvoltage", "#0" }, { "side", "#0" },
                };
                int skip = 0;
                for (i = 0; i < (int)(sizeof(def) / sizeof(def[0])); i++) {
                    if (!strcmp(key, def[i].k) && !strcasecmp(norm, def[i].v)) skip = 1;
                }
                if (strstr(key, "machine") == key && !strcmp(key, "machine") && !strcasecmp(norm, "off")) skip = 1;
                if (!skip) snprintf(out[n++], 160, "%s|%s|%s", sec, key, norm);
            }
        }
    }
    qsort(out, (size_t)n, 160, cmpl);
    return n;
}

static int compare(const char *name, const char *a, size_t al, const char *b, size_t bl)
{
    static char la[MAXL][160], lb[MAXL][160];
    int na = norm(a, al, la), nb = norm(b, bl, lb), i = 0, j = 0, diffs = 0;

    while (i < na || j < nb) {
        int c = i == na ? 1 : j == nb ? -1 : strcmp(la[i], lb[j]);
        if (c == 0) { i++; j++; continue; }
        diffs++;
        if (diffs <= 10) printf("  %s: %s %s\n", name, c < 0 ? "only in input:" : "only in output:", c < 0 ? la[i] : lb[j]);
        if (c < 0) i++; else j++;
    }
    return diffs;
}

/* ---- inputs ---- */

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return 0; }
    b[*n] = 0;
    fclose(f);
    return b;
}

static size_t crlf(const char *s, size_t n, char *o)
{
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') o[m++] = '\r';
        o[m++] = s[i];
    }
    return m;
}

/* Every stock key (REPORT.md section 2), 8 channels x 8 zones. */
static size_t full_stock(char *o, int stress)
{
    static const char *const ckeys[] = {
        "Pitch : +0.00", "Level : -3.5", "Pan : +0.25", "Attack : 0.0100", "AttackFromCurrent : 1",
        "Release : 0.1000", "Bits : 08.0", "Aliasing : 2", "PlayMode : 1", "MixLevel : -6.0",
        "PMSource : 3", "PMIndex : 0.50", "Reverse : 1", "Side : 1",
        "FooKey : 5", "BarKey : 6", "ChannelMode : 0", "LoopMode : 1",
        "LoopLengthIsEnd : 0", "AutoTrigger : 1", "LinAMisExtEnv : 0", "SpliceSmoothing : 1",
        "XfadeGroup : A", "ZonesRT : 1", "MixModIsFader : 0",
        "LinFM : 1A 0.50", "ExpFM : 2B -0.25", "PitchCV : 3C 1.00", "PhaseCV : 4A 0.10",
        "PMIndexMod : 5B 0.20", "BitsMod : 6C 0.30", "AliasingMod : 7A 0.40", "LinAM : 8B 0.50",
        "ExpAM : 1C 0.60", "AttackMod : 2A 0.70", "ReleaseMod : 3B 0.80", "PanMod : 4C -0.90",
        "SampleStartMod : 5A 1.00", "SampleEndMod : 6B 1.10", "LoopStartMod : 7C 1.20",
        "LoopLengthMod : 8A 1.30", "ZonesCV : 1B 1.40", "MixMod : 2C 1.50",
        "decay : 0.2000", "Pitch : +30.00", "Level : +20.0", "Bits : 0.5", "ChannelMode : 9",
    };
    size_t n = 0;
    n += (size_t)sprintf(o + n, "Preset 7 :\n  Name : Full Test\n  Data2asCV : 3B\n  XfadeACV : 1A\n"
                         "  XfadeAWidth : 0.50\n  MidiSetup : 0\n  FooBar : hello world\n");
    for (int c = 1; c <= 8; c++) {
        size_t nk = stress ? sizeof(ckeys) / sizeof(ckeys[0]) : 43;
        n += (size_t)sprintf(o + n, "  Channel %d :\n", c);
        for (size_t i = 0; i < nk; i++) {
            if (c > 1 && (i == 0 || i >= 43)) continue;
            n += (size_t)sprintf(o + n, "    %s\n", ckeys[i]);
        }
        for (int z = 1; z <= 8; z++) {
            n += (size_t)sprintf(o + n, "    Zone %d :\n      Sample : a-rather-long-sample-name-%d-%d.wav\n"
                                 "      SampleStart : %d.0000\n      SampleEnd : 123456.0000\n"
                                 "      LoopStart : 10.0000\n      LoopLength : 2000.0000\n"
                                 "      PitchOffset : +0.00\n      LevelOffset : -1.5\n"
                                 "      MinVoltage : -5.00\n", z, c, z, z);
        }
    }
    return n;
}

static int one_file(const char *name, const char *buf, size_t len, size_t *maxkeep)
{
    static struct preset p, p2;
    static char out[1 << 17], out2[1 << 17];
    int r, w, w2, diffs;

    kinit();
    r = yml_parse(buf, len, &p, KEEP);
    CHECK(r == YML_OK, "%s: parse %d", name, r);
    if (r) return 0;
    if (KEEP->used > *maxkeep) *maxkeep = KEEP->used;
    CHECK(KEEP->lost == 0, "%s: lost %u", name, KEEP->lost);
    w = yml_write(&p, KEEP, 7, out, sizeof(out));
    CHECK(w > 0, "%s: write %d", name, w);
    if (w <= 0) return 0;
    diffs = compare(name, buf, len, out, (size_t)w);
    CHECK(diffs == 0, "%s: %d semantic diffs", name, diffs);
    kinit();
    CHECK(yml_parse(out, (size_t)w, &p2, KEEP) == YML_OK, "%s: reparse", name);
    CHECK(!memcmp(&p, &p2, sizeof(p)), "%s: struct differs on reparse", name);
    w2 = yml_write(&p2, KEEP, 7, out2, sizeof(out2));
    CHECK(w2 == w && !memcmp(out, out2, (size_t)w), "%s: write not idempotent", name);
    return 1;
}

/* ---- struct round trip ---- */

static unsigned rng_state = 12345;
static unsigned rnd(unsigned n)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (rng_state >> 8) % n;
}

static int rnd_range(int lo, int hi) { return lo + (int)rnd((unsigned)(hi - lo + 1)); }

static void random_sampler(struct sampler_cfg *s)
{
    sampler_defaults(s);
    if (rnd(3) == 0) return;
    s->attack = rnd(3) ? 0 : rnd(ENV_MAX + 1);
    s->release = rnd(3) ? 1000 : rnd(ENV_MAX + 1);
    s->play_mode = (uint8_t)rnd(2);
    s->loop_mode = (uint8_t)rnd(3);
    s->loop_is_end = (uint8_t)rnd(2);
    s->reverse = (uint8_t)rnd(2);
    s->attack_from_current = (uint8_t)rnd(2);
    s->splice = (uint8_t)rnd(2);
    s->zones_rt = (uint8_t)rnd(4);
    s->mixmod_fader = (uint8_t)rnd(2);
    s->linam_ext_env = (uint8_t)rnd(2);
    s->aliasing = (uint8_t)rnd(101);
    s->pm_source = (uint8_t)rnd(11);
    s->xfade = (uint8_t)rnd(5);
    s->zones_cv = (uint8_t)rnd(CV_SRC_COUNT);
    s->rev_cv = (uint8_t)rnd(CV_SRC_COUNT);
    s->pm_index = (int16_t)rnd(101);
    s->start_delay = (uint16_t)(rnd(2) ? 0 : rnd(SDELAY_MAX + 1));
    for (int i = 0; i < SM_COUNT; i++) {
        if (rnd(3) == 0) {
            s->mod[i].src = (uint8_t)(1 + rnd(CV_SRC_COUNT - 1));
            s->mod[i].amt = (int16_t)rnd_range(-AMT_MAX, AMT_MAX);
        }
    }
    for (int z = 0; z < ZONES; z++) {
        struct zone_cfg *q = &s->zone[z];
        if (rnd(2)) continue;
        q->start = rnd(2) ? 0 : (uint32_t)rnd(1u << 30) * 2u;
        q->end = rnd(2) ? POS_END : (uint32_t)rnd(1u << 30) * 2u + 1u;
        q->loop_start = rnd(2) ? 0 : (uint32_t)rnd(1u << 24);
        q->loop_len = rnd(2) ? POS_END : (uint32_t)rnd(1u << 30) + (uint32_t)rnd(1u << 30);
        q->pitch = (int16_t)rnd_range(-9600, 6000);
        q->level = (int16_t)rnd_range(-900, 60);
        q->minv = (int16_t)rnd_range(-500, 500);
        q->side = (uint8_t)rnd(2);
    }
}

static void random_eq(struct eq_value *e, int zone)
{
    eq_value_defaults(e);
    if (zone ? rnd(2) : rnd(3) == 0) {
        if (zone) memset(e, 0, sizeof(*e));
        return;
    }
    e->override = (uint8_t)zone;
    e->on = (uint8_t)rnd(2);
    e->hpf = (uint16_t)(rnd(2) ? 0 : rnd_range(EQ_HPF_MIN, EQ_HPF_MAX));
    for (int b = 0; b < 3; b++) {
        e->frequency[b] = (uint16_t)rnd(EQ_STEP_MAX + 1);
        e->q[b] = (uint8_t)(rnd(2) ? 0 : rnd(EQ_Q_STEPS + 1));
        e->gain[b] = (int16_t)rnd_range(-EQ_GAIN_MAX, EQ_GAIN_MAX);
    }
    eq_limit(e);
    for (int m = 0; m < TA_EQ_MODS; m++) {
        if (rnd(2)) {
            e->source[m] = (int8_t)rnd_range(TA_SRC_OFF, EQ_CV_SOURCE_MAX);
            e->amount[m] = (int8_t)rnd_range(-100, 100);
        }
    }
}

static void random_filter(struct filter_value *f, int zone)
{
    filter_value_defaults(f);
    if (zone ? rnd(2) : rnd(3) == 0) {
        if (zone) memset(f, 0, sizeof(*f));
        return;
    }
    f->override = (uint8_t)zone;
    f->type = (uint8_t)rnd(FILTER_TYPES);
    f->cutoff = (uint16_t)rnd(EQ_STEP_MAX + 1);
    f->resonance = (uint8_t)rnd(101);
    f->extra = (uint8_t)rnd(101);
    f->right = (int16_t)(rnd(2) ? 0 : rnd_range(-EQ_STEP_MAX, EQ_STEP_MAX));
    for (int m = 0; m < TA_FILTER_MODS; m++) {
        if (rnd(2)) {
            f->source[m] = (int8_t)rnd_range(TA_SRC_OFF, EQ_CV_SOURCE_MAX);
            f->amount[m] = (int8_t)rnd_range(-100, 100);
        }
    }
}

static void random_grain(struct grain_value *g)
{
    grain_value_defaults(g);
    if (rnd(2)) return;
    for (int f = 0; f < GRAIN_FIELDS; f++)
        if (rnd(2)) g->value[f] = (int16_t)rnd_range(grain_ranges[f].low, grain_ranges[f].high);
    for (int r = 0; r < GRAIN_ROUTES; r++) {
        if (rnd(3)) continue;
        g->source[r] = (int8_t)rnd_range(TA_SRC_OFF, EQ_CV_SOURCE_MAX);
        g->amount[r] = (int8_t)rnd_range(-100, 100);
    }
}

static void random_tl(struct channel_cfg *ch)
{
    struct timelock_value *t = &ch->tl;

    timelock_value_defaults(t);
    for (int z = 0; z < ZONES; z++) ch->s.zone[z].beats = (uint8_t)(rnd(3) ? 0 : rnd(TIME_LOCK_BEATS_MAX + 1));
    if (rnd(2)) return;
    t->on = (uint8_t)rnd(2);
    if (rnd(2)) return;
    t->tempo = (uint8_t)rnd(TIME_LOCK_TEMPOS);
    t->speed = (int16_t)(rnd(2) ? 0 : rnd_range(TIME_LOCK_SPEED_MIN, TIME_LOCK_SPEED_MAX));
    t->input = (int8_t)(rnd(2) ? TA_SRC_OFF : rnd_range(TA_SRC_OFF, EQ_CV_SOURCE_MAX));
    t->ppqn = (uint8_t)(rnd(2) ? 0 : rnd(TIME_LOCK_PPQN_MAX + 1));
}

static void random_preset(struct preset *p)
{
    memset(p, 0, sizeof(*p));
    p->magic = PRESET_MAGIC;
    p->version = PRESET_VERSION;
    p->d2cv = (uint8_t)rnd(25);
    for (int g = 0; g < XFADE_GROUPS; g++) {
        p->xfade_cv[g] = (uint8_t)rnd(25);
        p->xfade_width[g] = (int16_t)(rnd(2) ? XFW_DEFAULT : rnd_range(XFW_MIN, XFW_MAX));
    }
    for (unsigned i = 0, n = rnd(PRESET_NAME_LEN + 1); i < n; i++) p->name[i] = (char)rnd_range(33, 126);
    if (rnd(2)) {
        for (int i = 0; i < TA_MIXFX_BYTES; i++) p->mixfx[i] = (uint8_t)(rnd(4) ? 0 : rnd(256));
    }
    if (rnd(2)) {
        p->reverb_unit = rnd(2) ? REVERB_80 : REVERB_70;
        p->reverb_program = (uint8_t)rnd_range(1, p->reverb_unit == REVERB_80 ? 200 : 84);
    }
    for (int e = 0; e < REVERB_EDITS; e++) {
        int slot = e < (p->reverb_unit == REVERB_70 ? R70_EDITS : p->reverb_unit == REVERB_80 ? R80_EDITS : 0);

        p->reverb_edit[e] = (int16_t)(slot && rnd(3) ? rnd(128) : -1);
        p->reverb_route[e].source = (signed char)(slot && !rnd(4) ? rnd(24) : TA_SRC_OFF);
        p->reverb_route[e].amount = (signed char)(p->reverb_route[e].source >= 0 ? rnd_range(-100, 100) : 0);
    }
    for (int c = 0; c < CHANNELS; c++) {
        struct channel_cfg *ch = &p->ch[c];
        const struct voice_desc *v;
        ch->machine = (uint8_t)(rnd(4) ? rnd(VOICE_COUNT) : 0);
        v = voices[ch->machine];
        ch->mix_on = (uint8_t)rnd(2);
        ch->mode = (uint8_t)rnd(MODE_COUNT);
        ch->auto_trig = (uint8_t)rnd(2);
        ch->pitch = (int16_t)rnd_range(SPITCH_MIN, SPITCH_MAX);
        ch->level = (int16_t)rnd_range(SLEVEL_MIN, LEVEL_MAX);
        ch->mix = (int16_t)rnd_range(LEVEL_MIN, LEVEL_MAX);
        ch->pan = (int16_t)rnd_range(-100, 100);
        ch->bits = (int16_t)rnd_range(BITS_MIN, BITS_MAX);
        for (int d = 0; d < D_COUNT; d++) {
            ch->mod[d].amt = AMT_DEFAULT(d);
            if (d >= D_CTRL && (!v || (unsigned)(d - D_CTRL) >= v->nparams)) continue;
            if (rnd(2)) {
                ch->mod[d].src = (uint8_t)rnd(CV_SRC_COUNT);
                ch->mod[d].amt = (int16_t)rnd_range(-AMT_MAX, AMT_MAX);
                if (ch->mod[d].src == CV_OFF) ch->mod[d].amt = AMT_DEFAULT(d);
            }
        }
        for (unsigned i = 0; i < CONTROLS; i++) {
            ch->ctrl[i] = 64;
            if (v && i < v->nparams) ch->ctrl[i] = (uint8_t)rnd(128);
        }
        random_sampler(&ch->s);
        random_eq(&ch->eq, 0);
        random_filter(&ch->filter, 0);
        random_grain(&ch->grain);
        random_tl(ch);
        for (int i = 0; i < PO_COUNT; i++) {
            static const int16_t lim[PO_COUNT] = { 15600, 1020, 1020, 310, 459, 459, 100, 100, 5000,
                                                   360, 360, 360, 127, 127, 127, 127, 127, 127, 127, 127 };
            ch->ofs[i] = (int16_t)(rnd(3) ? 0 : rnd_range(-lim[i], lim[i]));
        }
        for (int z = 0; z < ZONES; z++) {
            random_eq(&ch->s.zone[z].eq, 1);
            random_filter(&ch->s.zone[z].filter, 1);
        }
        if (v && rnd(3) == 0) {
            for (unsigned i = 0; i < v->nparams; i++) ch->ctrl[i] = (uint8_t)v->params[i].def;
        }
    }
}

static int roundtrip(const char *name, const struct preset *p)
{
    static struct preset q;
    static char out[1 << 16];
    int w, r;

    kinit();
    w = yml_write(p, KEEP, 5, out, sizeof(out));
    CHECK(w > 0, "%s: write %d", name, w);
    if (w <= 0) return 0;
    r = yml_parse(out, (size_t)w, &q, KEEP);
    CHECK(r == YML_OK, "%s: parse %d", name, r);
    CHECK(KEEP->used == 0 && KEEP->lost == 0, "%s: kept %u lost %u", name, KEEP->used, KEEP->lost);
    if (memcmp(p, &q, sizeof(q))) {
        failures++;
        printf("FAIL %s: struct differs\n", name);
        for (int c = 0; c < CHANNELS; c++) {
            if (memcmp(&p->ch[c], &q.ch[c], sizeof(q.ch[c]))) printf("  channel %d differs\n", c + 1);
        }
        if (strcmp(p->name, q.name)) printf("  name '%s' vs '%s'\n", p->name, q.name);
        return 0;
    }
    return 1;
}

/* ---- fuzz ---- */

static int in_range(const struct preset *p)
{
    if (p->d2cv > 24) return 0;
    for (int g = 0; g < XFADE_GROUPS; g++)
        if (p->xfade_cv[g] > 24 || p->xfade_width[g] < XFW_MIN || p->xfade_width[g] > XFW_MAX) return 0;
    for (int c = 0; c < CHANNELS; c++) {
        const struct channel_cfg *ch = &p->ch[c];
        if (ch->machine >= VOICE_COUNT || ch->mode >= MODE_COUNT) return 0;
        if (ch->pitch < SPITCH_MIN || ch->pitch > SPITCH_MAX || ch->level < SLEVEL_MIN || ch->level > LEVEL_MAX) return 0;
        if (ch->mix < LEVEL_MIN || ch->mix > LEVEL_MAX || ch->pan < -100 || ch->pan > 100) return 0;
        if (ch->bits < BITS_MIN || ch->bits > BITS_MAX || ch->mix_on > 1) return 0;
        for (int i = 0; i < CONTROLS; i++) if (ch->ctrl[i] > 127) return 0;
        {
            const struct sampler_cfg *sc = &ch->s;
            if (sc->attack > ENV_MAX || sc->release > ENV_MAX || sc->play_mode > 1 || sc->loop_mode > 2) return 0;
            if (sc->zones_rt > 3 || sc->aliasing > 100 || sc->pm_source > 10 || sc->xfade > 4) return 0;
            if (sc->zones_cv >= CV_SRC_COUNT || sc->rev_cv >= CV_SRC_COUNT || sc->pm_index < 0 || sc->pm_index > 100) return 0;
            if (sc->start_delay > SDELAY_MAX) return 0;
            for (int z = 0; z < ZONES; z++) {
                const struct zone_cfg *q = &sc->zone[z];
                if (q->start > POS_END || q->end > POS_END || q->loop_start > POS_END || q->side > 1) return 0;
                if (q->pitch < -9600 || q->pitch > 6000 || q->level < -900 || q->level > 60) return 0;
                if (q->minv < -500 || q->minv > 500) return 0;
            }
            for (int i = 0; i < SM_COUNT; i++)
                if (sc->mod[i].src >= CV_SRC_COUNT || sc->mod[i].amt < -AMT_MAX || sc->mod[i].amt > AMT_MAX) return 0;
        }
        for (int d = 0; d < D_COUNT; d++) {
            if (ch->mod[d].src >= CV_SRC_COUNT || ch->mod[d].amt < -AMT_MAX || ch->mod[d].amt > AMT_MAX) return 0;
        }
        for (int z = -1; z < ZONES; z++) {
            const struct eq_value *e = z < 0 ? &ch->eq : &ch->s.zone[z].eq;
            const struct filter_value *f = z < 0 ? &ch->filter : &ch->s.zone[z].filter;
            if (e->on > 1 || (e->hpf && (e->hpf < EQ_HPF_MIN || e->hpf > EQ_HPF_MAX)) || e->reserved) return 0;
            for (int b = 0; b < 3; b++)
                if (e->frequency[b] > EQ_STEP_MAX || e->q[b] > EQ_Q_STEPS || e->gain[b] < -EQ_GAIN_MAX || e->gain[b] > EQ_GAIN_MAX) return 0;
            for (int m = 0; m < TA_EQ_MODS; m++)
                if (e->source[m] < TA_SRC_OFF || e->source[m] > EQ_CV_SOURCE_MAX || e->amount[m] < -100 || e->amount[m] > 100) return 0;
            if (f->type >= FILTER_TYPES || f->cutoff > EQ_STEP_MAX || f->resonance > 100 || f->extra > 100) return 0;
            if (f->right < -EQ_STEP_MAX || f->right > EQ_STEP_MAX) return 0;
        }
        for (int f = 0; f < GRAIN_FIELDS; f++)
            if (ch->grain.value[f] < grain_ranges[f].low || ch->grain.value[f] > grain_ranges[f].high) return 0;
        for (int r = 0; r < GRAIN_ROUTES; r++)
            if (ch->grain.source[r] < TA_SRC_OFF || ch->grain.source[r] > EQ_CV_SOURCE_MAX ||
                ch->grain.amount[r] < -100 || ch->grain.amount[r] > 100) return 0;
    }
    return p->name[PRESET_NAME_LEN] == 0;
}

static long fuzz_runs;

static void fuzz_one(const char *buf, size_t len, uint32_t cap)
{
    static struct preset p;
    static char out[1 << 17];
    char *copy = malloc(len ? len : 1);
    int r;

    memcpy(copy, buf, len);
    yml_keep_init(KEEP, cap);
    r = yml_parse(copy, len, &p, KEEP);
    free(copy);
    fuzz_runs++;
    {
        static struct yml_samples sm;
        int found = yml_samples(KEEP, &sm);
        CHECK(found >= 0 && found <= CHANNELS * YML_ZONES, "fuzz: samples %d", found);
        for (int c = 0; c < CHANNELS; c++)
            for (int z = 0; z < YML_ZONES; z++) CHECK(strlen(sm.name[c][z]) <= YML_SAMPLE_LEN, "fuzz: sample name");
    }
    if (r == YML_OK) {
        int w;
        char *tight;
        CHECK(in_range(&p), "fuzz: out of range");
        w = yml_write(&p, KEEP, 1, out, sizeof(out));
        CHECK(w > 0, "fuzz: write %d", w);
        if (w > 0) {
            tight = malloc((size_t)w);
            CHECK(yml_write(&p, KEEP, 1, tight, (size_t)w) == w, "fuzz: tight write");
            CHECK(yml_write(&p, KEEP, 1, tight, (size_t)w - 1) == YML_E_SPACE, "fuzz: short write");
            free(tight);
        }
    } else {
        CHECK(r == YML_E_FORMAT, "fuzz: parse %d", r);
    }
}

/* ---- 4. zone samples, WAV headers ---- */

static const char *hook_name(unsigned smp)
{
    return smp == 1 ? "one.wav" : smp == 2 ? "two.wav" : 0;
}

static void test_samples(void)
{
    static const char src[] =
        "Preset 1 :\n  Name : s\n  Channel 1 :\n    Level : -3.0\n    Zone 1 :\n      Sample : kick.wav\n"
        "    Zone 2 :\n      SAMPLE : Snare Hit.WAV  \n      MinVoltage : -1.00\n"
        "  Channel 3 :\n    Zone 8 :\n      Sample : late.wav\n      Sample : later.wav\n"
        "  Channel 4 :\n    Sample : notazone.wav\n    Zone 1 :\n      Sample : \n"
        "  Channel 5 :\n    Zone 1 :\n      Sample : 0123456789012345678901234567890123456789012345678.wav\n"
        "  Channel 6 :\n    Zone 1 :\n      Sample : 01234567890123456789012345678901234567890ab.wav\n"
        "  Channel 8 :\n    Machine : TRX-BD\n    Zone 1 :\n      Sample : tone.wav\n";
    static struct preset p;
    static struct yml_samples sm;
    static char out[1 << 16];
    int w, n;

    printf("== 4. zone samples\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "samples: parse");
    n = yml_samples(KEEP, &sm);
    CHECK(n == 5, "samples: found %d", n);
    CHECK(!strcmp(sm.name[0][0], "kick.wav"), "samples: [0][0] '%s'", sm.name[0][0]);
    CHECK(!strcmp(sm.name[0][1], "Snare Hit.WAV"), "samples: [0][1] '%s'", sm.name[0][1]);
    CHECK(!strcmp(sm.name[2][7], "later.wav"), "samples: [2][7] '%s'", sm.name[2][7]);
    CHECK(!sm.name[3][0][0], "samples: empty value / channel-level key");
    CHECK(!sm.name[4][0][0], "samples: 49-char name kept");
    CHECK(strlen(sm.name[5][0]) == 47, "samples: 47-char name dropped");
    CHECK(!strcmp(sm.name[7][0], "tone.wav"), "samples: machine channel zone");
    CHECK(!sm.name[1][0][0] && !sm.name[0][2][0], "samples: phantom");
    /* The zone lines still round-trip verbatim. */
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0 && strstr(out, "      SAMPLE : Snare Hit.WAV\n") && strstr(out, "      MinVoltage : -1.00\n"),
          "samples: written back");
    kinit();
    CHECK(w > 0 && yml_parse(out, (size_t)w, &p, KEEP) == YML_OK && yml_samples(KEEP, &sm) == 5 &&
          !strcmp(sm.name[2][7], "later.wav"), "samples: reparse");
    CHECK(yml_samples(NULL, &sm) == 0 && !sm.name[0][0][0], "samples: NULL keep");
    /* Firmware: names come from zone_cfg.smp; zones without one are left out. */
    yml_sample_name = hook_name;
    kinit();
    yml_parse(src, sizeof(src) - 1, &p, KEEP);
    p.ch[0].s.zone[0].smp = 2;
    p.ch[0].s.zone[1].smp = 0;
    p.ch[2].s.zone[7].smp = 1;
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0 && strstr(out, "    Zone 1 :\n      Sample : two.wav\n") && !strstr(out, "Snare Hit") &&
          !strstr(out, "kick.wav") && strstr(out, "    Zone 8 :\n      Sample : one.wav\n") &&
          !strstr(out, "later.wav"), "samples: hook");
    yml_sample_name = 0;
    printf("  ok\n");
}

static void test_sampler(void)
{
    static const char src[] =
        "Preset 1 :\n  Channel 1 :\n    Attack :  0.0123\n    Release : 99.0000\n    PlayMode : 1\n"
        "    LoopMode : 2\n    MLoopWrap : 1\n    ZonesCV : 1A\n    MReverseCV : 2A\n    SampleStart : 100\n    Reverse : 1\n    XfadeGroup : c\n"
        "    ExpFM : 0A 0.50\n    Zone 1 :\n      Sample : a.wav\n      LoopLength :    100.5000\n"
        "    Zone 2 :\n      Sample : b.wav\n      SampleStart : 12.0000\n      LoopLength : 100.0312\n"
        "      MinVoltage : -5.00\n      Side : 1\n      PitchOffset : -12.00\n      LevelOffset : -90.0\n"
        "  Channel 2 :\n    Attack : 100.0000\n    Zone 1 :\n      LoopLength : 100.0100\n      SampleEnd : 2147483647\n"
        "      SampleStart : 1.5\n";
    static struct preset p;
    static char out[1 << 16];
    const struct sampler_cfg *a, *b;
    int w;

    printf("== 4. sampler keys\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "sampler: parse");
    a = &p.ch[0].s;
    b = &p.ch[1].s;
    CHECK(a->attack == 123 && a->release == 990000 && a->play_mode == 1 && a->loop_mode == 2,
          "sampler: env/modes");
    CHECK(a->rev_cv == CV_ABS + 3 && a->zones_cv == CV_ABS && a->reverse == 1 && a->xfade == 3, "sampler: zones cv/reverse/xfade");
    CHECK(a->mod[SM_EXPFM].src == CV_REL_A && a->mod[SM_EXPFM].amt == 50, "sampler: ExpFM");
    CHECK(a->zone[0].start == 100 && a->zone[0].loop_len == 3216, "sampler: channel-level start -> zone 1");
    CHECK(a->zone[1].start == 12 && a->zone[1].loop_len == 3201 && a->zone[1].minv == -500 &&
          a->zone[1].side == 1 && a->zone[1].pitch == -1200 && a->zone[1].level == -900, "sampler: zone 2");
    CHECK(b->attack == 0 && b->zone[0].loop_len == POS_END && b->zone[0].end == POS_END && b->zone[0].start == 0,
          "sampler: kept / defaults");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "sampler: write");
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, "    Attack :  0.0123\n") && strstr(out, "    Release : 99.0000\n"), "sampler: env format");
        CHECK(strstr(out, "    Zone 1 :\n      Sample : a.wav\n      SampleStart : 100\n      LoopLength :    100.5000\n") != 0,
              "sampler: zone 1 format\n%s", out);
        CHECK(strstr(out, "      LoopLength :    100.0312\n") && strstr(out, "      LoopLength : 100.0100\n"), "sampler: loop lengths");
        CHECK(strstr(out, "      SampleStart : 1.5\n") && !strstr(out, "SampleEnd"), "sampler: verbatim / default end");
        CHECK(strstr(out, "    MLoopWrap : 1\n") && !strstr(strstr(out, "MLoopWrap") + 1, "MLoopWrap"),
              "sampler: old MLoopWrap kept verbatim");
        CHECK(strstr(out, "    ZonesCV : 1A\n    MReverseCV : 2A\n") && strstr(out, "    XfadeGroup : C\n"), "sampler: zones cv / reverse cv / xfade");
    }
    printf("  ok\n");
}

/* Machine channels: PlayMode defaults to 1-SHOT and is written only when
 * GATED; the envelope and Aliasing keys are the sampler's. */
/* Stereo/Right offsets and START DELAY: our keys, parsed, written, kept by
 * a reparse; out-of-range values are kept as lines. */
static void test_pair_keys(void)
{
    static const char src[] =
        "Preset 1 :\n  Channel 1 :\n    MStartDelay : 12.5\n    MStartDelayMod : 0B 0.50\n"
        "  Channel 2 :\n    ChannelMode : 2\n    MRightPitch : +7.00\n    MRightLevel : -1.5\n"
        "    MRightStartDelay : +0.5\n    MRightAttack : -3\n    MRightControl3 : +12\n    MRightEQGain2 : -4.5\n"
        "    MRightMixLevel : 999.0\n";
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== pair offsets / start delay\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "pair: parse");
    CHECK(p.ch[0].s.start_delay == 125 && p.ch[0].s.mod[SM_DELAY].src == CV_REL_B && p.ch[0].s.mod[SM_DELAY].amt == 50,
          "pair: start delay %d", p.ch[0].s.start_delay);
    CHECK(p.ch[1].ofs[PO_PITCH] == 700 && p.ch[1].ofs[PO_LEVEL] == -15 && p.ch[1].ofs[PO_DELAY] == 5 &&
          p.ch[1].ofs[PO_ATTACK] == -3 && p.ch[1].ofs[PO_CTRL + 2] == 12 && p.ch[1].ofs[PO_EQGAIN + 1] == -45 &&
          p.ch[1].ofs[PO_MIX] == 0, "pair: offsets");
    CHECK(!preset_has_offsets(&p.ch[0]) && preset_has_offsets(&p.ch[1]), "pair: has offsets");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "pair: write");
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, "    MStartDelay : 12.5\n") && strstr(out, "    MStartDelayMod : 0B 0.50\n"), "pair: delay keys\n%s", out);
        CHECK(strstr(out, "    MRightPitch : +7.00\n") && strstr(out, "    MRightLevel : -1.5\n") &&
              strstr(out, "    MRightStartDelay : +0.5\n") && strstr(out, "    MRightControl3 : +12\n") &&
              strstr(out, "    MRightEQGain2 : -4.5\n") && strstr(out, "    MRightMixLevel : 999.0\n"),
              "pair: offset keys\n%s", out);
        CHECK(yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "pair: reparse");
    }
}

static void test_machine_play(void)
{
    static const char src[] =
        "Preset 1 :\n  Channel 1 :\n    Machine : TRX-BD\n"
        "  Channel 2 :\n    PlayMode : 0\n    Attack :  0.2000\n    Release :  0.0500\n    Aliasing : 100\n"
        "    AttackMod : 0B 0.50\n    AliasingMod : 0C -1.00\n    Machine : EFM-SD\n"
        "  Channel 3 :\n    PlayMode : 1\n    Machine : PI-BD\n"
        "  Channel 4 :\n    Level : -1.0\n";
    static struct preset p, p2;
    static char out[1 << 16];
    int w;

    printf("== 4b. machine play mode / envelope\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK && KEEP->used == 0, "mplay: parse");
    CHECK(p.ch[0].machine && p.ch[0].s.play_mode == PLAY_ONESHOT && p.ch[0].s.attack == 0, "mplay: default 1-SHOT");
    CHECK(p.ch[1].s.play_mode == PLAY_GATED && p.ch[1].s.attack == 2000 && p.ch[1].s.release == 500 &&
          p.ch[1].s.aliasing == 100 && p.ch[1].s.mod[SM_ATTACK].src == CV_REL_B &&
          p.ch[1].s.mod[SM_ALIAS].amt == -100, "mplay: gated / env / aliasing");
    CHECK(p.ch[2].s.play_mode == PLAY_ONESHOT, "mplay: explicit 1-SHOT");
    CHECK(!p.ch[3].machine && p.ch[3].s.play_mode == PLAY_GATED, "mplay: sample channel default GATED");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "mplay: write");
    if (w > 0) {
        out[w] = 0;
        const char *c2 = strstr(out, "Channel 2"), *c3 = strstr(out, "Channel 3");
        CHECK(c2 && c3 && !strstr(out, "PlayMode : 1"), "mplay: 1-SHOT machine omits PlayMode\n%s", out);
        CHECK(c2 && strstr(c2, "    PlayMode : 0\n") && strstr(c2, "    PlayMode : 0\n") < c3, "mplay: GATED written\n%s", out);
        CHECK(yml_parse(out, (size_t)w, &p2, KEEP) == YML_OK && !memcmp(&p, &p2, sizeof(p)), "mplay: round trip");
    }
    /* A machine picked on a sample channel takes the machine default, and back. */
    preset_set_machine(&p.ch[3], 1);
    CHECK(p.ch[3].s.play_mode == PLAY_ONESHOT, "mplay: set machine -> 1-SHOT");
    p.ch[3].s.play_mode = PLAY_GATED;
    preset_set_machine(&p.ch[3], 2);
    CHECK(p.ch[3].s.play_mode == PLAY_GATED, "mplay: machine to machine keeps");
    preset_set_machine(&p.ch[3], 0);
    CHECK(p.ch[3].s.play_mode == PLAY_GATED, "mplay: back to samples -> GATED");
    /* Sustaining (MNM) machines default to GATED; drum <-> MNM takes the new default. */
    int mm = 0;
    for (int m = 1; m < VOICE_COUNT && !mm; m++) mm = voices[m]->flags & VF_GATED ? m : 0;
    CHECK(mm, "mplay: a GATED machine");
    preset_set_machine(&p.ch[3], 1);
    preset_set_machine(&p.ch[3], mm);
    CHECK(p.ch[3].s.play_mode == PLAY_GATED, "mplay: drum -> MNM GATED");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    if (w > 0) {
        out[w] = 0;
        const char *c4 = strstr(out, "Channel 4");
        CHECK(c4 && !strstr(c4, "PlayMode"), "mplay: GATED MNM omits PlayMode\n%s", out);
        CHECK(yml_parse(out, (size_t)w, &p2, KEEP) == YML_OK && !memcmp(&p, &p2, sizeof(p)), "mplay: MNM round trip");
    }
    p.ch[3].s.play_mode = PLAY_ONESHOT;
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, "PlayMode : 1"), "mplay: 1-SHOT MNM written\n%s", out);
        CHECK(yml_parse(out, (size_t)w, &p2, KEEP) == YML_OK && !memcmp(&p, &p2, sizeof(p)), "mplay: MNM 1-SHOT round trip");
    }
    preset_set_machine(&p.ch[3], 1);
    CHECK(p.ch[3].s.play_mode == PLAY_ONESHOT, "mplay: MNM -> drum 1-SHOT");
    printf("  ok\n");
}

static void test_xfade(void)
{
    static const char src[] = "Preset 1 :\n  XfadeACV : 1A\n  XfadeAWidth : 2.50\n  xfadebcv : 3c\n"
                              "  XfadeCCV : Off\n  XfadeDWidth : 10.00\n  XfadeCWidth : 0.01\n";
    static struct preset p;
    static char out[1 << 16];
    int w;

    printf("== 4. crossfade keys\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK && KEEP->used == 0, "xfade: parse");
    CHECK(p.xfade_cv[0] == 1 && p.xfade_cv[1] == 9 && p.xfade_cv[2] == 0 && p.xfade_cv[3] == 0, "xfade: cv");
    CHECK(p.xfade_width[0] == 250 && p.xfade_width[1] == XFW_DEFAULT && p.xfade_width[2] == 1 &&
          p.xfade_width[3] == 1000, "xfade: width");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "xfade: write");
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, "  XfadeACV : 1A\n  XfadeAWidth : 2.50\n  XfadeBCV : 3C\n  XfadeCWidth : 0.01\n"
                          "  XfadeDWidth : 10.00\n") != 0, "xfade: format\n%s", out);
        CHECK(!strstr(out, "XfadeBWidth") && !strstr(out, "XfadeCCV"), "xfade: defaults omitted");
    }
    kinit();
    static const char bad[] = "Preset 1 :\n  XfadeEWidth : 1.00\n  XfadeAWidth : 0.00\n  XfadeACV : 0A\n";
    CHECK(yml_parse(bad, sizeof(bad) - 1, &p, KEEP) == YML_OK &&
          KEEP->used > 0 && p.xfade_width[0] == XFW_DEFAULT && p.xfade_cv[0] == 0, "xfade: bad values kept");
    printf("  ok\n");
}

/* Tallent Edition's lines (its PRESET_FORMAT.md example, plus zone and
 * right-offset forms): read into the preset, the others kept, all written
 * back after the channels behind ExtrasFormat. */
static void test_tallent(void)
{
    static const char src[] =
        "Preset 1 :\n  Name : TALLENT\n  Channel 1 :\n    Zone 1 :\n      Sample : a.wav\n"
        "  ExtrasFormat : 3\n"
        "  EQ1 : 1 80 150 30 0 1000 -45 12 10000 0 0\n"
        "  EQCV1 : -4 100 -4 100 0 50 -4 100 -4 100 -4 100 -4 100\n"
        "  FILT1 : 1 800 55 50\n"
        "  FILT1Z1 : 6 3000 55 50\n"
        "  Grain1 : 1 800 400 20 0 0 25 50 100 0 0 -60\n"
        "  TimeLock1 : 1 3 0 -4 0\n"
        "  TimeLock2Z1 : 8\n"
        "  EQ3Z2 : 0 0 150.35 0 0 1000 20 5 10022.71 -100 0\n"
        "  FILTR2 : -1200\n"
        "  FILT4 : 1 800 55\n";
    /* Frequencies come back as their step's, whole hertz where that reads
     * back the same (Tallent's writer does the same). */
    static const char expect[] =
        "  ExtrasFormat : 3\n"
        "  EQ1 : 1 80 150 30 0 1000 -45 12 10021 0 0\n"
        "  EQCV1 : -4 100 -4 100 0 50 -4 100 -4 100 -4 100 -4 100\n"
        "  FILT1 : 1 798 55 50\n"
        "  FILT1Z1 : 6 2997 55 50\n"
        "  FILTR2 : -1200\n"
        "  EQ3Z2 : 0 0 150 0 0 1000 20 5 10021 -100 0\n"
        "  Grain1 : 1 800 400 20 0 0 25 50 100 0 0 -60\n"
        "  TimeLock1 : 1 3 0 -4 0\n"
        "  TimeLock2Z1 : 8\n"
        "  FILT4 : 1 800 55\n";
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== 4. Tallent lines\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "tallent: parse");
    const struct eq_value *e = &p.ch[0].eq;
    CHECK(e->on == 1 && e->hpf == 80 && e->frequency[0] == eq_step_nearest(15000) && e->gain[0] == 30 &&
          e->gain[1] == -45 && e->q[1] == 12 && e->frequency[2] == eq_step_nearest(1000000), "tallent: EQ1");
    CHECK(e->source[2] == 0 && e->amount[2] == 50 && e->source[0] == TA_SRC_OFF, "tallent: EQCV1");
    CHECK(p.ch[0].filter.type == 1 && p.ch[0].filter.cutoff == eq_step_nearest(80000) &&
          p.ch[0].filter.resonance == 55 && !p.ch[0].filter.override, "tallent: FILT1");
    CHECK(p.ch[0].s.zone[0].filter.override && p.ch[0].s.zone[0].filter.type == 6, "tallent: FILT1Z1");
    CHECK(!p.ch[0].s.zone[1].filter.override && !p.ch[0].s.zone[0].eq.override, "tallent: zones follow");
    CHECK(p.ch[2].s.zone[1].eq.override && p.ch[2].s.zone[1].eq.gain[2] == -100 &&
          p.ch[2].s.zone[1].eq.frequency[0] == EQ_STEP_150HZ, "tallent: EQ3Z2");
    CHECK(p.ch[1].filter.right == -120 && p.ch[1].filter.type == 0, "tallent: FILTR2");
    CHECK(p.ch[3].filter.type == 0, "tallent: short FILT4 not ours");
    const struct grain_value *g = &p.ch[0].grain;
    CHECK(g->value[GRAIN_ON] == 1 && g->value[GRAIN_SIZE] == 800 && g->value[GRAIN_OVERLAP] == 400 &&
          g->value[GRAIN_SPRAY] == 20 && g->value[GRAIN_REVERSE] == 25 && g->value[GRAIN_PAN] == 50 &&
          g->value[GRAIN_SHAPE] == 100 && g->value[GRAIN_SCAN] == 0 && g->value[GRAIN_SKEW] == -60, "tallent: Grain1");
    CHECK(p.ch[1].grain.value[GRAIN_SIZE] == 600 && p.ch[1].grain.source[0] == TA_SRC_OFF, "tallent: Grain2 default");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "tallent: write");
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, expect) != 0 && strstr(out, expect) + strlen(expect) == out + w, "tallent: lines\n%s", out);
        kinit();
        CHECK(yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "tallent: again");
    }
    kinit();
    static const char only[] = "Preset 1 :\n  Channel 2 :\n    Level : +1.0\n  Grain2 : 1 800 400 20 0 0 25 50 100 0 0 -60\n";
    CHECK(yml_parse(only, sizeof(only) - 1, &p, KEEP) == YML_OK, "tallent: grain only");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && strstr(out, "  ExtrasFormat : 3\n  Grain2 : 1 800"), "tallent: kept lines get the format line\n%s", out);
    printf("  ok\n");
}

/* MixFX lines (the mix effects' settings as bytes): Tallent's defaults
 * with channel 1 compressed (4:1, sidechain channel 1), sends 30/25 Pre with
 * a reverb send route (CV A, +0.60), delay tempo 60 BPM. Read into
 * preset.mixfx, written last as Tallent does, after kept lines; a
 * malformed line is kept. */
/* Grain lines: Grain<c>/GrainCV<c> (clamped to the ranges), format 1's
 * Grain/GrainCV for every channel (Grain's first number the playing
 * channel), malformed ones and Grain9 kept; written per channel after the
 * EQ and filter lines, at the defaults not at all. */
static void test_grain(void)
{
    static const char src[] =
        "Preset 1 :\n  Channel 1 :\n    Level : +1.0\n"
        "  Grain : 3 300 800 0 50 25 0 100 60 50 -500 20\n"
        "  GrainCV : 0 100 -4 100 -3 -50 -4 100 2 40 -4 100 -4 100 -4 100\n"
        "  Grain2 : 1 9999 900 20 0 0 0 50 0 100 0 0\n"
        "  GrainCV4 : -4 100 -4 100 -4 100 -4 100 -4 100 -4 100 -4 100 -4 100\n"
        "  Grain5 : 1 800 400\n"
        "  Grain9 : 1 800 400 20 0 0 25 50 100 0 0 -60\n"
        "  EQ1 : 1 80 150 30 0 1000 -45 12 10000 0 0\n";
    static const char expect[] =
        "  ExtrasFormat : 3\n"
        "  EQ1 : 1 80 150 30 0 1000 -45 12 10021 0 0\n"
        "  Grain1 : 0 300 800 0 50 25 0 100 60 50 -500 20\n"
        "  GrainCV1 : 0 100 -4 100 -3 -50 -4 100 2 40 -4 100 -4 100 -4 100\n"
        "  Grain2 : 1 9999 800 20 0 0 0 50 1 100 0 0\n"
        "  GrainCV2 : 0 100 -4 100 -3 -50 -4 100 2 40 -4 100 -4 100 -4 100\n"
        "  Grain3 : 1 300 800 0 50 25 0 100 60 50 -500 20\n"
        "  GrainCV3 : 0 100 -4 100 -3 -50 -4 100 2 40 -4 100 -4 100 -4 100\n"
        "  Grain4 : 0 300 800 0 50 25 0 100 60 50 -500 20\n"
        "  Grain5 : 0 300 800 0 50 25 0 100 60 50 -500 20\n"
        "  GrainCV5 : 0 100 -4 100 -3 -50 -4 100 2 40 -4 100 -4 100 -4 100\n";
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== 4a. Grain lines\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "grain: parse");
    CHECK(p.ch[2].grain.value[GRAIN_ON] == 1 && p.ch[0].grain.value[GRAIN_ON] == 0 && p.ch[7].grain.value[GRAIN_SIZE] == 300,
          "grain: format 1 into every channel");
    CHECK(p.ch[1].grain.value[GRAIN_SIZE] == 9999 && p.ch[1].grain.value[GRAIN_OVERLAP] == 800 &&
          p.ch[1].grain.value[GRAIN_SHAPE] == 1, "grain: Grain2 clamped");
    CHECK(p.ch[3].grain.source[0] == TA_SRC_OFF && p.ch[6].grain.source[0] == 0 && p.ch[6].grain.amount[2] == -50,
          "grain: routes");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "grain: write");
    if (w > 0) {
        out[w] = 0;
        const char *at = strstr(out, expect);
        CHECK(at && strstr(at, "  Grain5 : 1 800 400\n") && strstr(at, "  Grain9 : 1 800"), "grain: lines\n%s", out);
        CHECK(strstr(out, "Grain6 : 0 300") && strstr(out, "GrainCV8 :"), "grain: other channels\n%s", out);
        kinit();
        CHECK(yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "grain: again");
    }
    printf("  ok\n");
}

/* TimeLock lines: PRESET_FORMAT.md's (MIDI clock on channel 1, Beats 8 for
 * channel 2's zone 1), short lines (absent numbers 0, input Off), clamping,
 * malformed ones and TimeLock9 kept; written after the grain lines, the
 * channel's line with all five numbers once its tempo is set. */
static void test_timelock(void)
{
    static const char src[] =
        "Preset 1 :\n  Channel 1 :\n    Level : +1.0\n"
        "  ExtrasFormat : 3\n"
        "  TimeLock1 : 1 3 0 -4 0\n"
        "  TimeLock2Z1 : 8\n"
        "  TimeLock3 : 1\n"
        "  TimeLock4 : 0 4 -100 -2 24\n"
        "  TimeLock5 : 1 2\n"
        "  TimeLock6 : 1 9 5000 99 200\n"
        "  TimeLock7 : 1 2 3 4 5 6\n"
        "  TimeLock7Z9 : 4\n"
        "  TimeLock8Z2 : 300\n"
        "  TimeLock8Z3 : 2 2\n"
        "  TimeLock9 : 1\n";
    static const char expect[] =
        "  ExtrasFormat : 3\n"
        "  TimeLock1 : 1 3 0 -4 0\n"
        "  TimeLock2Z1 : 8\n"
        "  TimeLock3 : 1\n"
        "  TimeLock4 : 0 4 -100 -2 24\n"
        "  TimeLock5 : 1 2 0 -4 0\n"
        "  TimeLock6 : 1 4 3000 23 96\n"
        "  TimeLock8Z2 : 128\n"
        "  TimeLock7 : 1 2 3 4 5 6\n"
        "  TimeLock7Z9 : 4\n"
        "  TimeLock8Z3 : 2 2\n"
        "  TimeLock9 : 1\n";
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== 4c. TimeLock lines\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "timelock: parse");
    const struct timelock_value *t = &p.ch[0].tl;
    CHECK(t->on == 1 && t->tempo == TIME_LOCK_TEMPO_MIDI && t->speed == 0 && t->input == TA_SRC_OFF && t->ppqn == 0,
          "timelock: TimeLock1");
    CHECK(p.ch[1].s.zone[0].beats == 8 && !p.ch[1].tl.on && !p.ch[1].s.zone[1].beats, "timelock: TimeLock2Z1");
    CHECK(p.ch[2].tl.on && !p.ch[2].tl.tempo && p.ch[2].tl.input == TA_SRC_OFF, "timelock: short line");
    t = &p.ch[3].tl;
    CHECK(!t->on && t->tempo == TIME_LOCK_TEMPO_CV && t->speed == -100 && t->input == -2 && t->ppqn == 24,
          "timelock: TimeLock4");
    t = &p.ch[5].tl;
    CHECK(t->tempo == 4 && t->speed == 3000 && t->input == 23 && t->ppqn == 96, "timelock: clamped");
    CHECK(!p.ch[6].tl.on && p.ch[7].s.zone[1].beats == 128 && !p.ch[7].s.zone[2].beats, "timelock: malformed kept");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "timelock: write");
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, expect) != 0 && strstr(out, expect) + strlen(expect) == out + w, "timelock: lines\n%s", out);
        kinit();
        CHECK(yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "timelock: again");
    }
    printf("  ok\n");
}

static void test_mixfx(void)
{
    static const char lines[] =
        "  MixFX1 : 1 4 20 4 3 0 0 100 1 0 20 4 3 0 0 100\n"
        "  MixFX2 : 0 0 20 4 3 0 0 100 0 0 20 4 3 0 0 100\n"
        "  MixFX3 : 0 0 20 4 3 0 0 100 0 0 20 4 3 0 0 100\n"
        "  MixFX4 : 0 0 20 4 3 0 0 100 0 0 20 4 3 0 0 100\n"
        "  MixFX5 : 0 0 20 4 3 0 0 100 0 30 0 0 0 0 0 0\n"
        "  MixFX6 : 0 25 0 0 0 0 0 0 0 1 25 20 80 40 50 5\n"
        "  MixFX7 : 45 3 2 80 29 253 60 252 0 252 0 252 0 252 0 252\n"
        "  MixFX8 : 0 252 0 252 0 252 0 252 0 252 0 252 0 252 0 252\n"
        "  MixFX9 : 0 252 0 252 0 252 0 252 0 252 0 252 0 252 0 252\n"
        "  MixFX10 : 0 252 0 252 0 252 0 252 0 1 0 0 0 0 0 0\n";
    static const char src_head[] = "Preset 1 :\n  Name : MIXFX\n  Channel 1 :\n    Level : +1.0\n";
    static char src[4096], expect[4096];
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== 4b. MixFX lines\n");
    snprintf(src, sizeof(src), "%s  ExtrasFormat : 3\n  MixFX12 : 1 2 3\n  TimeLock1 : 1 3 0 -4 0\n%s", src_head, lines);
    snprintf(expect, sizeof(expect), "  ExtrasFormat : 3\n  TimeLock1 : 1 3 0 -4 0\n  MixFX12 : 1 2 3\n%s", lines);
    kinit();
    CHECK(yml_parse(src, strlen(src), &p, KEEP) == YML_OK, "mixfx: parse");
    CHECK(p.mixfx[0] == 1 && p.mixfx[1] == 4 && p.mixfx[8] == 1 && p.mixfx[73] == 30 && p.mixfx[81] == 25 &&
          p.mixfx[89] == 1 && p.mixfx[100] == 29 && p.mixfx[101] == 253 && p.mixfx[102] == 60 && p.mixfx[153] == 1 &&
          p.mixfx[160] == 0 && p.mixfx[176] == 0, "mixfx: bytes");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    CHECK(w > 0, "mixfx: write");
    if (w > 0) {
        out[w] = 0;
        CHECK(strstr(out, expect) != 0 && strstr(out, expect) + strlen(expect) == out + w, "mixfx: lines\n%s", out);
        kinit();
        CHECK(yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "mixfx: again");
    }
    kinit();
    static const char only[] = "Preset 1 :\n  Channel 1 :\n    Level : +1.0\n  MixFX16 : 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 7\n";
    CHECK(yml_parse(only, sizeof(only) - 1, &p, KEEP) == YML_OK && p.mixfx[255] == 7, "mixfx: last line");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && strstr(out, "  ExtrasFormat : 3\n  MixFX16 : 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 7\n"), "mixfx: format line\n%s", out);
    kinit();
    static const char dclk[] = "Preset 1 :\n  Channel 1 :\n    Level : +1.0\n  MixFX11 : 0 210 24 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    CHECK(yml_parse(dclk, sizeof(dclk) - 1, &p, KEEP) == YML_OK && p.mixfx[161] == 210 && p.mixfx[162] == 24, "mixfx: CV CLOCK bytes");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && strstr(out, "  MixFX11 : 0 210 24 0 0 0 0 0 0 0 0 0 0 0 0 0\n"), "mixfx: CV CLOCK line\n%s", out);
    kinit();
    static const char none[] = "Preset 1 :\n  Channel 1 :\n    Level : +1.0\n  MixFX3 : 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    CHECK(yml_parse(none, sizeof(none) - 1, &p, KEEP) == YML_OK, "mixfx: zeros");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && !strstr(out, "MixFX") && !strstr(out, "ExtrasFormat"), "mixfx: all zero is the defaults, unwritten\n%s", out);
    printf("  ok\n");
}

/* The mix reverb's unit and program (ours): written when not the DEP-5's. */
static void test_reverb_unit(void)
{
    static const char src[] = "Preset 1 :\n  ReverbUnit : 80\n  ReverbProgram : 170\n  Channel 1 :\n    Level : +1.0\n";
    static const char bad[] = "Preset 1 :\n  ReverbUnit : 71\n  ReverbProgram : 0\n  Channel 1 :\n    Level : +1.0\n";
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== 4c. ReverbUnit, ReverbProgram\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK && p.reverb_unit == REVERB_80 && p.reverb_program == 170,
          "reverb: parse");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && strstr(out, "  ReverbUnit : 80\n  ReverbProgram : 170\n"), "reverb: lines\n%s", out);
    kinit();
    CHECK(w > 0 && yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "reverb: again");
    kinit();
    CHECK(yml_parse(bad, sizeof(bad) - 1, &p, KEEP) == YML_OK && !p.reverb_unit && !p.reverb_program, "reverb: bad");
    p.reverb_unit = 0;
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && !strstr(out, "  ReverbUnit : 80"), "reverb: DEP-5 unwritten\n%s", out);
    {
        static const char s70[] = "Preset 1 :\n  ReverbUnit : 70\n  ReverbProgram : 21\n  ReverbRtMid : 12\n"
                                  "  ReverbRtMidMod : 1A 0.50\n  Channel 1 :\n    Level : +1.0\n";
        kinit();
        CHECK(yml_parse(s70, sizeof(s70) - 1, &p, KEEP) == YML_OK && p.reverb_unit == REVERB_70 &&
                  p.reverb_program == 21 && p.reverb_edit[R70_E_RTMID] == 12 &&
                  p.reverb_route[R70_E_RTMID].source == 0 && p.reverb_route[R70_E_RTMID].amount == 50,
              "reverb 70: parse");
        w = yml_write(&p, KEEP, 1, out, sizeof(out));
        out[w > 0 ? w : 0] = 0;
        CHECK(w > 0 && strstr(out, "  ReverbUnit : 70\n  ReverbProgram : 21\n  ReverbRtMid : 12\n  ReverbRtMidMod : 1A"),
              "reverb 70: lines\n%s", out);
        kinit();
        CHECK(w > 0 && yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "reverb 70: again");
    }
    printf("  ok\n");
}

/* The 80's edits and their CV routes: Reverb<key>, Reverb<key>Mod. */
static void test_reverb_edits(void)
{
    static const char src[] = "Preset 1 :\n  ReverbUnit : 80\n  ReverbProgram : 170\n  ReverbMidRt : 40\n"
                              "  ReverbAdjust : 0\n  ReverbMidRtMod : 3B -0.50\n  ReverbFXMixMod : 8C 1.00\n"
                              "  ReverbSizeMod : A 0.50\n  ReverbBogus : 3\n  Channel 1 :\n    Level : +1.0\n";
    static struct preset p, q;
    static char out[1 << 16];
    int w;

    printf("== 4d. Reverb edits and routes\n");
    kinit();
    CHECK(yml_parse(src, sizeof(src) - 1, &p, KEEP) == YML_OK, "reverb edits: parse");
    CHECK(p.reverb_edit[R80_E_MIDRT] == 40 && p.reverb_edit[R80_E_ADJUST] == 0 && p.reverb_edit[R80_E_SIZE] == -1,
          "reverb edits: values");
    CHECK(p.reverb_route[R80_E_MIDRT].source == 7 && p.reverb_route[R80_E_MIDRT].amount == -50 &&
          p.reverb_route[R80_E_FXMIX].source == 23 && p.reverb_route[R80_E_FXMIX].amount == 100 &&
          p.reverb_route[R80_E_SIZE].source == TA_SRC_OFF && p.reverb_route[R80_E_ADJUST].source == TA_SRC_OFF,
          "reverb edits: routes");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    out[w > 0 ? w : 0] = 0;
    CHECK(w > 0 && strstr(out, "  ReverbAdjust : 0\n") && strstr(out, "  ReverbMidRt : 40\n  ReverbMidRtMod : 3B -0.50\n") &&
          strstr(out, "  ReverbFXMixMod : 8C 1.00\n") && !strstr(out, "ReverbSize :"),
          "reverb edits: lines\n%s", out);
    CHECK(strstr(out, "ReverbSizeMod : A 0.50") && strstr(out, "ReverbBogus : 3"), "reverb edits: kept\n%s", out);
    kinit();
    CHECK(w > 0 && yml_parse(out, (size_t)w, &q, KEEP) == YML_OK && !memcmp(&p, &q, sizeof(p)), "reverb edits: again");
    printf("  ok\n");
}

/* Stock's own save: every line we write is one stock wrote, and back. */
static void test_exact(const char *path)
{
    static struct preset p;
    static char out[1 << 16];
    static char la[MAXL][160], lb[MAXL][160];
    size_t n;
    char *b = slurp(path, &n);
    int w, na = 0, nb = 0;

    if (!b) return;
    printf("== 4. exact lines of %s\n", path);
    kinit();
    CHECK(yml_parse(b, n, &p, KEEP) == YML_OK, "exact: parse");
    w = yml_write(&p, KEEP, 1, out, sizeof(out));
    for (size_t i = 0, s0 = 0; i < n; i++)
        if (b[i] == '\n') { snprintf(la[na++], 160, "%.*s", (int)(i - s0), b + s0); s0 = i + 1; }
    for (int i = 0, s0 = 0; i < w; i++)
        if (out[i] == '\n') { snprintf(lb[nb++], 160, "%.*s", i - s0, out + s0); s0 = i + 1; }
    qsort(la, (size_t)na, 160, cmpl);
    qsort(lb, (size_t)nb, 160, cmpl);
    CHECK(na == nb, "exact: %d vs %d lines", na, nb);
    for (int i = 0; i < na && i < nb; i++) CHECK(!strcmp(la[i], lb[i]), "exact: '%s' vs '%s'", la[i], lb[i]);
    free(b);
    printf("  ok\n");
}

struct mem { const uint8_t *p; uint32_t n; };

static uint32_t mem_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    struct mem *m = ctx;
    if (off >= m->n) return 0;
    if (n > m->n - off) n = m->n - off;
    memcpy(buf, m->p + off, n);
    return n;
}

static size_t put32(uint8_t *o, uint32_t v) { o[0] = v; o[1] = v >> 8; o[2] = v >> 16; o[3] = v >> 24; return 4; }
static size_t put16(uint8_t *o, uint32_t v) { o[0] = v; o[1] = v >> 8; return 2; }

/* RIFF with an optional chunk before fmt; returns the length. */
static size_t mkwav(uint8_t *o, int tag, int ch, uint32_t rate, int bits, int ext, int ext_tag,
                    uint32_t data_len, uint32_t claim, const char *pre, uint32_t pre_len)
{
    size_t n = 12;
    int ba = ch * bits / 8;

    memcpy(o, "RIFF", 4);
    memcpy(o + 8, "WAVE", 4);
    if (pre) {
        memcpy(o + n, pre, 4);
        n += 4;
        n += put32(o + n, pre_len);
        if (pre_len < 4096) {   /* else the size is a lie */
            memset(o + n, 'x', pre_len);
            n += pre_len + (pre_len & 1);
        }
    }
    memcpy(o + n, "fmt ", 4);
    n += 4;
    n += put32(o + n, ext ? 40 : 16);
    n += put16(o + n, ext ? 0xfffe : tag);
    n += put16(o + n, ch);
    n += put32(o + n, rate);
    n += put32(o + n, rate * ba);
    n += put16(o + n, ba);
    n += put16(o + n, bits);
    if (ext) {
        static const uint8_t tail[14] = { 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71 };
        n += put16(o + n, 22);
        n += put16(o + n, bits);
        n += put32(o + n, 3);
        n += put16(o + n, ext_tag);
        memcpy(o + n, tail, 14);
        n += 14;
    }
    memcpy(o + n, "data", 4);
    n += 4;
    n += put32(o + n, claim);
    for (uint32_t i = 0; i < data_len; i++) o[n++] = (uint8_t)i;
    put32(o + 4, (uint32_t)n - 8);
    return n;
}

static const char *parse(const uint8_t *b, size_t n, struct wav_info *w)
{
    struct mem m = { b, (uint32_t)n };
    return wav_parse(mem_read, &m, (uint32_t)n, w);
}

static void test_wav(void)
{
    static uint8_t b[1 << 16];
    struct wav_info w;
    const char *e;
    size_t n;
    static const struct { int tag, ch, bits, ext, ext_tag; const char *want; } cases[] = {
        { 1, 1, 8, 0, 0, 0 }, { 1, 2, 16, 0, 0, 0 }, { 1, 1, 24, 0, 0, 0 }, { 1, 2, 32, 0, 0, 0 },
        { 3, 1, 32, 0, 0, 0 }, { 0, 2, 24, 1, 1, 0 }, { 0, 2, 32, 1, 3, 0 }, { 0, 1, 16, 1, 1, 0 },
        { 3, 2, 64, 0, 0, "UNSUPPORTED FORMAT" }, { 2, 1, 4, 0, 0, "UNSUPPORTED FORMAT" },
        { 1, 3, 16, 0, 0, "TOO MANY CHANNELS" }, { 1, 1, 12, 0, 0, "UNSUPPORTED BITS" },
        { 0, 1, 16, 1, 2, "UNSUPPORTED FORMAT" },
    };

    printf("== 4. wav headers\n");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int ba = cases[i].ch * cases[i].bits / 8;
        n = mkwav(b, cases[i].tag, cases[i].ch, 44100, cases[i].bits, cases[i].ext, cases[i].ext_tag,
                  ba * 100u, ba * 100u, 0, 0);
        e = parse(b, n, &w);
        if (cases[i].want) {
            CHECK(e && !strcmp(e, cases[i].want), "wav case %u: got %s", i, e ? e : "ok");
        } else {
            CHECK(!e && w.frames == 100 && w.channels == cases[i].ch && w.bits == cases[i].bits &&
                  w.rate == 44100 && w.block == ba && w.data_len == ba * 100u &&
                  w.is_float == (cases[i].tag == 3 || cases[i].ext_tag == 3) && w.data_off == n - ba * 100u,
                  "wav case %u: %s", i, e ? e : "fields");
        }
    }
    /* Odd-sized chunk before fmt (padded), odd data length (partial frame dropped). */
    n = mkwav(b, 1, 2, 48000, 16, 0, 0, 403, 403, "LIST", 27);
    e = parse(b, n, &w);
    CHECK(!e && w.frames == 100 && w.data_off == 12 + 8 + 28 + 24 + 8, "wav: odd chunk %s off %u", e ? e : "", w.data_off);
    /* Data claims more than the file holds: clipped. */
    n = mkwav(b, 1, 1, 48000, 16, 0, 0, 1000, 5000, 0, 0);
    e = parse(b, n, &w);
    CHECK(!e && w.frames == 500, "wav: clip %s %u", e ? e : "", w.frames);
    /* Chunks after the data chunk, and a huge unknown chunk size. */
    n = mkwav(b, 1, 1, 48000, 16, 0, 0, 1000, 1000, 0, 0);
    memcpy(b + n, "cue ", 4); put32(b + n + 4, 0xffffffffu); n += 8;
    CHECK(!parse(b, n, &w) && w.frames == 500, "wav: trailing chunk");
    n = mkwav(b, 1, 1, 48000, 16, 0, 0, 1000, 1000, "junk", 0xfffffff0u);
    CHECK(parse(b, n, &w) != 0, "wav: huge pre-chunk accepted");
    /* No data / empty data / not RIFF / not WAVE / short files. */
    n = mkwav(b, 1, 1, 48000, 16, 0, 0, 0, 0, 0, 0);
    e = parse(b, n, &w);
    CHECK(e && !strcmp(e, "EMPTY WAV"), "wav: empty %s", e ? e : "ok");
    n = mkwav(b, 1, 1, 48000, 16, 0, 0, 100, 100, 0, 0);
    memcpy(b, "RIFX", 4);
    CHECK(parse(b, n, &w) != 0, "wav: RIFX");
    memcpy(b, "RIFF", 4);
    memcpy(b + 8, "AVI ", 4);
    CHECK(parse(b, n, &w) != 0, "wav: AVI");
    memcpy(b + 8, "WAVE", 4);
    for (size_t l = 0; l < n; l++) {
        e = parse(b, l, &w);
        CHECK(e || (w.data_off + w.data_len <= l && w.frames), "wav: truncated at %zu", l);
    }
    /* Block align that doesn't match. */
    n = mkwav(b, 1, 2, 48000, 16, 0, 0, 100, 100, 0, 0);
    put16(b + 12 + 8 + 12, 3);
    CHECK(parse(b, n, &w) != 0, "wav: block align");
    /* Random garbage and bit flips never crash and never yield out-of-file data. */
    {
        uint8_t m[600];
        size_t base = mkwav(b, 1, 2, 48000, 24, 1, 1, 300, 300, "LIST", 9);
        for (int i = 0; i < 200000; i++) {
            size_t l = i & 1 ? rnd(sizeof(m)) : base;
            if (i & 1) {
                for (size_t k = 0; k < l; k++) m[k] = (uint8_t)rnd(256);
                if (rnd(2)) memcpy(m, "RIFF\0\0\0\0WAVE", l < 12 ? l : 12);
            } else {
                memcpy(m, b, base);
                for (unsigned k = 0, nf = 1 + rnd(6); k < nf; k++) m[rnd((unsigned)base)] = (uint8_t)rnd(256);
            }
            e = parse(m, l, &w);
            CHECK(e || (w.frames && w.data_off + w.data_len <= l && w.channels >= 1 && w.channels <= 2),
                  "wav fuzz %d", i);
        }
    }
    printf("  ok\n");
}


/* Writer output parses back to the same format and carries the frames. */
static void test_wav_write(void)
{
    static uint8_t b[1 << 14];
    static const struct { int ch, bits, flt; } cases[] = {
        { 1, 8, 0 }, { 2, 8, 0 }, { 1, 16, 0 }, { 2, 16, 0 }, { 1, 24, 0 }, { 2, 24, 0 }, { 1, 32, 0 },
        { 2, 32, 0 }, { 1, 32, 1 }, { 2, 32, 1 },
    };

    printf("== 5. wav writer\n");
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        for (uint32_t frames = 1; frames <= 7; frames += 2) {
            struct wav_info w = { 0 }, r;
            uint32_t hl, n, pad;
            const char *e;

            w.rate = 44100 + i * 1000;
            w.channels = (uint8_t)cases[i].ch;
            w.bits = (uint8_t)cases[i].bits;
            w.block = (uint8_t)(cases[i].ch * cases[i].bits / 8);
            w.is_float = (uint8_t)cases[i].flt;
            w.frames = frames;
            hl = wav_header(b, &w);
            n = frames * w.block;
            pad = n & 1;
            for (uint32_t k = 0; k < n; k++) b[hl + k] = (uint8_t)(k * 7 + 1);
            if (pad) b[hl + n] = 0;
            e = parse(b, hl + n + pad, &r);
            CHECK(!e && r.rate == w.rate && r.channels == w.channels && r.bits == w.bits && r.block == w.block &&
                  r.is_float == w.is_float && r.frames == frames && r.data_off == hl && r.data_len == n,
                  "wav write %u/%u: %s", i, frames, e ? e : "fields");
            CHECK((uint32_t)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24) == hl + n + pad - 8,
                  "wav write %u/%u: RIFF size", i, frames);
            CHECK(hl <= WAV_HEADER_MAX && !(hl & 1), "wav write %u: header %u", i, hl);
        }
    }
    printf("  ok\n");
}

static uint32_t xr = 12345;
static uint32_t xrnd(void) { xr = xr * 1664525u + 1013904223u; return xr >> 8; }

/* Decaying noise bursts at known positions over a quiet tone and noise. */
static void burst_signal(int32_t *l, int32_t *r, uint32_t len, const uint32_t *at, int nb, int tone)
{
    for (uint32_t i = 0; i < len; i++) {
        double v = tone ? 0.02 * sin(i * 0.05) : 0.0;
        v += ((int32_t)(xrnd() & 0xff) - 128) / 128.0 * 0.001;
        for (int k = 0; k < nb; k++) {
            if (i >= at[k] && i < at[k] + 6000) {
                v += (((int32_t)(xrnd() & 0xffff) - 32768) / 32768.0) * 0.8 * exp(-(double)(i - at[k]) / 1500.0);
            }
        }
        l[i] = (int32_t)(v * 2147483647.0);
        r[i] = r == l ? l[i] : (int32_t)(v * 0.5 * 2147483647.0);
    }
}

static void test_ops(void)
{
    static int32_t l[96000], r[96000];
    static const uint32_t at[] = { 5000, 22000, 31000, 60000, 73000 };
    uint32_t b[9];

    printf("== 6. slices and trim\n");
    slices_equal(100, 1100, 4, b);
    CHECK(b[0] == 100 && b[1] == 350 && b[2] == 600 && b[3] == 850 && b[4] == 1100, "equal slices");
    slices_equal(0, 7, 3, b);
    CHECK(b[0] == 0 && b[3] == 7 && b[1] < b[2] && b[2] < b[3], "equal slices odd");
    for (int tone = 0; tone < 2; tone++) {
        for (int st = 0; st < 2; st++) {
            burst_signal(l, st ? r : l, 96000, at, 5, tone);
            slices_chop(l, st ? r : l, 0, 96000, 48000, 6, b);
            CHECK(b[0] == 0 && b[6] == 96000, "chop ends");
            for (int k = 0; k < 5; k++) {
                CHECK(b[k + 1] >= at[k] - 300 && b[k + 1] <= at[k] + 100, "chop tone %d st %d: onset %d at %u (want %u)",
                      tone, st, k, b[k + 1], at[k]);
            }
            slices_chop(l, st ? r : l, 0, 96000, 48000, 3, b);
            CHECK(b[1] > 0 && b[2] > b[1] && b[3] == 96000, "chop 3");
            {
                int ok = 0;
                for (int k = 0; k < 5; k++) ok += b[1] >= at[k] - 300 && b[1] <= at[k] + 100;
                CHECK(ok, "chop 3: first point on an onset (%u)", b[1]);
            }
            slices_chop(l, st ? r : l, 2000, 90000, 48000, 8, b);
            CHECK(b[0] == 2000 && b[8] == 90000, "chop 8 ends");
            for (int k = 0; k < 8; k++) CHECK(b[k + 1] >= b[k] + 4, "chop 8 order");
        }
    }
    memset(l, 0, sizeof(l));
    slices_chop(l, l, 0, 96000, 48000, 4, b);
    CHECK(b[1] == 24000 && b[2] == 48000 && b[3] == 72000, "chop silence = equal");
    slices_chop(l, l, 10, 60, 48000, 8, b);
    CHECK(b[0] == 10 && b[8] == 60, "chop tiny");
    {
        struct zone_cfg z;

        zone_defaults(&z);
        z.start = 300; z.end = 900; z.loop_start = 400; z.loop_len = 100 * 32;
        zone_trim(&z, 300, 900, 1000);
        CHECK(z.start == 0 && z.end == POS_END && z.loop_start == 100 && z.loop_len == 100 * 32, "trim both");
        zone_defaults(&z);
        z.start = 300; z.end = 800; z.loop_start = 100;
        zone_trim(&z, 300, 1000, 1000);
        CHECK(z.start == 0 && z.end == 500 && z.loop_start == 0 && z.loop_len == POS_END, "trim start: %u %u %u %u",
              z.start, z.end, z.loop_start, z.loop_len);
        zone_defaults(&z);
        z.start = 100; z.loop_start = 50; z.loop_len = 400 * 32;
        zone_trim(&z, 0, 300, 1000);
        CHECK(z.start == 100 && z.end == POS_END && z.loop_start == 50 && z.loop_len == POS_END, "trim end");
        zone_defaults(&z);
        z.start = 800; z.end = 900;
        zone_trim(&z, 0, 500, 1000);
        CHECK(z.start == 499 && z.end == POS_END, "trim outside");
    }
    printf("  ok\n");
}

/* A stereo pair as tmp/srmirror/mkfold2.py: master ch1 zones stA/stB/stC
 * (smp 1/2/3) at +2/-1/-5 V, starts 100/200/300; ch2 Stereo/Right with the
 * R sides at +3/+1/-5 V, starts 1100/1200/1300 (rn zones). */
static void pair_setup(struct preset *p, int rn)
{
    static const int16_t mv[2][3] = { { 200, -100, -500 }, { 300, 100, -500 } };

    preset_init(p);
    p->ch[1].mode = MODE_STEREO;
    for (int c = 0; c < 2; c++) {
        for (int z = 0; z < (c ? rn : 3); z++) {
            struct zone_cfg *zc = &p->ch[c].s.zone[z];
            zc->smp = (uint16_t)(z + 1);
            zc->side = (uint8_t)c;
            zc->start = (uint32_t)(c * 1000 + (z + 1) * 100);
            zc->minv = z + 1 == (c ? rn : 3) ? -500 : mv[c][z];
        }
    }
}

static const char *pair_name(unsigned smp)
{
    static const char *const n[] = { "stA.wav", "stB.wav", "stC.wav", "mono1.wav", "mono2.wav" };
    return smp >= 1 && smp <= 5 ? n[smp - 1] : 0;
}

/* Save -> reload keeps both sides' zone lists (names mapped back to smp). */
static void pair_roundtrip(const char *what, const struct preset *p)
{
    static struct preset q;
    static struct yml_samples sm;
    static char out[1 << 16];
    int w, ok = 1;

    yml_sample_name = pair_name;
    kinit();
    w = yml_write(p, KEEP, 1, out, sizeof(out));
    yml_sample_name = 0;
    kinit();
    CHECK(w > 0 && yml_parse(out, (size_t)w, &q, KEEP) == YML_OK, "%s: reparse", what);
    yml_samples(KEEP, &sm);
    for (int c = 0; c < 2; c++) {
        for (int z = 0; z < ZONES; z++) {
            unsigned s = 0;
            for (unsigned k = 1; k <= 5; k++) {
                s = !strcmp(sm.name[c][z], pair_name(k)) ? k : s;
            }
            q.ch[c].s.zone[z].smp = (uint16_t)s;
            ok &= !memcmp(&q.ch[c].s.zone[z], &p->ch[c].s.zone[z], sizeof(struct zone_cfg));
        }
        ok &= q.ch[c].mode == p->ch[c].mode;
    }
    CHECK(ok, "%s: pair differs after save/reload", what);
}

static void test_zone_pair(void)
{
    static struct preset p;
    struct sampler_cfg *m = &p.ch[0].s, *r = &p.ch[1].s;
    struct zone_cfg z;

    printf("== 6b. Stereo/Right zone lists (stock observations, tmp/srmirror)\n");
    /* E1: "++" with stereo stB: the new zone follows; each side halves its own range. */
    pair_setup(&p, 3);
    zone_defaults(&z);
    z.smp = 2;
    z.start = 300;
    CHECK(zones_append(m, &z) == 3, "append index");
    CHECK(zones_sr_follow(m, r, 3, 0, 3, 1), "append followed");
    CHECK(zones_count(r) == 4 && r->zone[3].smp == 2 && r->zone[3].side == 1 && r->zone[3].start == 300 &&
          r->zone[3].minv == -500, "append: S/R zone 4 copy");
    CHECK(m->zone[2].minv == -300 && r->zone[2].minv == -200, "append halves own: %d %d", m->zone[2].minv,
          r->zone[2].minv);
    pair_roundtrip("append", &p);
    /* E4: the S/R has fewer zones: unchanged. */
    pair_setup(&p, 2);
    z.smp = 4;
    zones_append(m, &z);
    CHECK(!zones_sr_follow(m, r, 3, 0, 3, 0) && zones_count(r) == 2 && r->zone[1].minv == -500, "append short S/R");
    /* Mono append with equal counts: same file, Side 0. */
    pair_setup(&p, 3);
    zones_append(m, &z);
    CHECK(zones_sr_follow(m, r, 3, 0, 3, 0) && r->zone[3].smp == 4 && !r->zone[3].side, "append mono");
    /* E5: delete a middle zone: both shift, own values kept. */
    pair_setup(&p, 3);
    zones_delete(m, 1);
    zones_delete(r, 1);
    CHECK(zones_count(m) == 2 && zones_count(r) == 2 && r->zone[0].minv == 300 && r->zone[1].smp == 3 &&
          r->zone[1].start == 1300 && r->zone[1].minv == -500, "delete middle");
    pair_roundtrip("delete", &p);
    /* Delete the last: the new last zone is -5 V. */
    pair_setup(&p, 3);
    zones_delete(m, 2);
    CHECK(zones_count(m) == 2 && m->zone[1].minv == -500 && m->zone[0].minv == 200, "delete last");
    /* E6: insert at zone 2: the copy takes the upper half of the zone's range, per side. */
    pair_setup(&p, 3);
    zones_insert(m, 1);
    zones_insert(r, 1);
    CHECK(zones_count(m) == 4 && m->zone[1].minv == 50 && m->zone[2].minv == -100 && m->zone[1].smp == 2 &&
          m->zone[2].smp == 2 && m->zone[3].smp == 3, "insert master");
    CHECK(r->zone[1].minv == 200 && r->zone[2].minv == 100 && r->zone[2].start == 1200, "insert S/R");
    pair_roundtrip("insert", &p);
    /* Insert with all 8 used: the last is lost, the new last is -5 V. */
    preset_init(&p);
    for (int k = 0; k < ZONES; k++) {
        m->zone[k].smp = (uint16_t)(k % 5 + 1);
        m->zone[k].minv = (int16_t)(400 - k * 100);
    }
    zones_insert(m, 2);
    CHECK(zones_count(m) == 8 && m->zone[2].minv == 250 && m->zone[3].minv == 200 && m->zone[7].minv == -500 &&
          m->zone[7].smp == 2, "insert full");
    CHECK(zones_append(m, &z) < 0, "append full");
    /* E8: a new sample on an existing master zone: S/R copies it (positions, MinVoltage). */
    pair_setup(&p, 3);
    m->zone[0].smp = 2;
    CHECK(zones_sr_follow(m, r, 0, 1, 3, 1) && r->zone[0].smp == 2 && r->zone[0].side == 1 &&
          r->zone[0].start == 100 && r->zone[0].minv == 200, "change stereo");
    m->zone[2].smp = 5;
    CHECK(zones_sr_follow(m, r, 2, 3, 3, 0) && r->zone[2].smp == 5 && !r->zone[2].side, "change to mono");
    r->zone[1].smp = 4;
    m->zone[1].smp = 5;
    CHECK(!zones_sr_follow(m, r, 1, 2, 3, 0) && r->zone[1].smp == 4, "mono: independent S/R zone kept");
    pair_roundtrip("change", &p);
    /* Flip keeps the voltage ranges on each side. */
    pair_setup(&p, 3);
    zones_flip(m, 0, 3);
    zones_flip(r, 0, 3);
    CHECK(m->zone[0].smp == 3 && m->zone[2].smp == 1 && m->zone[0].minv == 200 && r->zone[0].start == 1300 &&
          r->zone[0].minv == 300 && r->zone[2].minv == -500, "flip");
    printf("  ok\n");
}

/* Descriptors restate the mdkit voices' names and defaults; they must agree. */
static void check_descriptors(void)
{
    int seen[VOICE_COUNT] = { 0 };

    for (int m = 1; m < VOICE_COUNT; m++) {
        const struct voice_desc *v = voices[m];
        const mdkit_voice *o = v->ops;
        CHECK(!strcmp(v->name, o->name) && v->nparams == o->control_count && v->nparams <= CONTROLS,
              "voice %d: descriptor vs mdkit", m);
        for (unsigned i = 0; i < v->nparams && i < o->control_count; i++) {
            const struct voice_param *q = &v->params[i];
            CHECK(!strcmp(q->name, o->control_names[i]) &&
                  q->def == (o->control_defaults ? o->control_defaults[i] : 64) &&
                  q->min <= q->def && q->def <= q->max && q->tag <= TAG_LEVEL,
                  "voice %s param %u", v->name, i);
        }
        CHECK(!(v->flags & VF_PITCH_IN) || ((const struct pitched_voice *)o)->set_pitch, "voice %s: pitch input", v->name);
    }
    for (int f = 0; f < FAMILY_COUNT; f++) {
        for (unsigned i = 0; i < families[f].count; i++) {
            int m = families[f].ids[i];
            CHECK(m > 0 && m < VOICE_COUNT && voices[m]->family == f && !seen[m]++, "family %s", families[f].name);
        }
    }
    for (int m = 1; m < VOICE_COUNT; m++) CHECK(seen[m] == 1, "voice %d in no family", m);
}

/* 1:A8M-SSETUP.YML (src/ssetup.c): every field round-trips; a stock-era
 * file leaves the monitor fields at their defaults; bad values are ignored. */
static void test_ssetup(void)
{
    static const struct rec_setup zero;
    char t[512];
    int ok = 0;

    printf("== ssetup\n");
    for (int i = 0; i < 5000; i++) {
        struct rec_setup a = zero, b = zero;
        a.dest = (uint8_t)(rand() % (RDEST_EMPTY + 1));
        a.zone_empty = (uint8_t)(rand() % 2);
        a.mode = (uint8_t)(rand() % RMODE_COUNT);
        a.time = (int16_t)(rand() % (RTIME_MAX + 1));
        a.rate = (uint8_t)(rand() % 3);
        a.naming = (uint8_t)(rand() % 3);
        a.hpf = (uint8_t)(rand() % 2);
        a.hot = (uint8_t)(rand() % 2);
        a.thresh = (int16_t)(RTHRESH_MIN + rand() % (25 - RTHRESH_MIN));
        a.side = (uint8_t)(rand() % 4);
        a.level = (int8_t)(RLEVEL_MIN + rand() % (RLEVEL_MAX - RLEVEL_MIN + 1));
        a.mon_l = (uint8_t)(rand() % 2);
        a.mon_r = (uint8_t)(rand() % 2);
        a.mon_level = (int8_t)(RMON_OFF + rand() % (RMON_MAX - RMON_OFF + 1));
        a.mon_rev = (uint8_t)(rand() % (RMON_SEND_MAX + 1));
        a.mon_dly = (uint8_t)(rand() % (RMON_SEND_MAX + 1));
        size_t n = ssetup_text(&a, t, sizeof(t));
        CHECK(n && ssetup_text(&a, t, n) == 0, "ssetup_text cap");
        ssetup_parse(t, n, &b);
        if (n && !memcmp(&a, &b, sizeof(a))) ok++;
        else if (ok == i) printf("  first mismatch:\n%s", t);
    }
    CHECK(ok == 5000, "ssetup round trip %d/5000", ok);
    {
        static const char stock[] = "Destination : 3\r\nThreshold : 2B\nSide : 3\nLevel : -6\nMonitorL : 1\n"
                                    "MonitorLevel : -61\nMonitorReverb : 101\nMonitorDelay : x\nTime : 5\n";
        struct rec_setup r = zero;
        ssetup_parse(stock, sizeof(stock) - 1, &r);
        CHECK(r.dest == 3 && r.thresh == 5 && r.side == RSIDE_DUAL && r.level == -6 && r.mon_l == 1 &&
              r.mon_level == 0 && r.mon_rev == 0 && r.mon_dly == 0 && r.time == 5, "ssetup stock-era file");
        size_t n = ssetup_text(&r, t, sizeof(t));
        CHECK(n && strstr(t, "Threshold : 2B\n") && strstr(t, "MonitorLevel : 0\n") && strstr(t, "MonitorReverb : 0\n"),
              "ssetup text");
        r.thresh = 0;
        ssetup_text(&r, t, sizeof(t));
        CHECK(strstr(t, "Threshold : Off\n") != 0, "ssetup Off");
    }
    printf("  %d/5000 round trips ok\n", ok);
}

int main(int argc, char **argv)
{
    static char big[1 << 17], big2[1 << 17];
    static struct preset p;
    size_t maxkeep = 0, n;
    int files = 0, prs = 0;
    char *seeds[64];
    size_t seedlen[64];
    int nseeds = 0;

    if (argc < 2) return 2;
    check_descriptors();

    printf("== 1. stock yml files\n");
    for (int i = 2; i < argc; i++) {
        char *b = slurp(argv[i], &n);
        if (!b) { printf("cannot read %s\n", argv[i]); failures++; continue; }
        files += one_file(argv[i], b, n, &maxkeep);
        {
            char *c = malloc(n * 2 + 1);
            size_t m = crlf(b, n, c);
            files += one_file(argv[i], c, m, &maxkeep);
            free(c);
        }
        if (nseeds < 60) { seeds[nseeds] = b; seedlen[nseeds++] = n; } else free(b);
    }
    for (int s = 0; s < 2; s++) {
        n = full_stock(big, s);
        files += one_file(s ? "full stock + out-of-range" : "full stock 8x8", big, n, &maxkeep);
        kinit();
        CHECK(yml_parse(big, n, &p, KEEP) == YML_OK && p.ch[0].auto_trig && p.ch[7].auto_trig,
              "AutoTrigger not parsed");
        printf("  %s: %zu bytes, kept %u\n", s ? "stress" : "full", n, KEEP->used);
        n = crlf(big, n, big2);
        files += one_file("full stock CRLF", big2, n, &maxkeep);
        seeds[nseeds] = malloc(n); memcpy(seeds[nseeds], big, n); seedlen[nseeds++] = n;
    }
    printf("  %d inputs ok (of %d runs counted), max kept pool use %zu bytes\n", files, 2 * (argc - 2) + 4, maxkeep);

    printf("== 2. struct round trips\n");
    {
        DIR *d = opendir(argv[1]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char path[1024];
            FILE *f;
            if (e->d_name[0] != 'P' || !strstr(e->d_name, ".PRS")) continue;
            snprintf(path, sizeof(path), "%s/%s", argv[1], e->d_name);
            f = fopen(path, "rb");
            if (f && fread(&p, sizeof(p), 1, f) == 1) {
                prs += roundtrip(e->d_name, &p);
                {
                    static char out[1 << 16];
                    int w;
                    kinit();
                    w = yml_write(&p, KEEP, 1, out, sizeof(out));
                    if (prs == 1 && w > 0) printf("  sample (%s, %d bytes):\n%.*s\n", e->d_name, w, w > 700 ? 700 : w, out);
                }
            }
            if (f) fclose(f);
        }
        if (d) closedir(d);
        printf("  %d example PRS ok\n", prs);
    }
    {
        int ok = 0;
        for (int i = 0; i < 20000; i++) {
            char nm[32];
            snprintf(nm, sizeof(nm), "random %d", i);
            random_preset(&p);
            ok += roundtrip(nm, &p);
        }
        printf("  %d/20000 random presets ok\n", ok);
    }

    printf("== 3. fuzz\n");
    for (int s = 0; s < nseeds; s++) {
        size_t step = seedlen[s] > 2000 ? 13 : 1;
        for (size_t l = 0; l <= seedlen[s]; l += step) fuzz_one(seeds[s], l, YML_KEEP_FULL);
        for (int i = 0; i < 400; i++) {
            char *m = malloc(seedlen[s] ? seedlen[s] : 1);
            memcpy(m, seeds[s], seedlen[s]);
            for (unsigned k = 0, nf = 1 + rnd(8); seedlen[s] && k < nf; k++) m[rnd((unsigned)seedlen[s])] = (char)rnd(256);
            fuzz_one(m, seedlen[s], rnd(3) ? YML_KEEP_FULL : rnd(64));
            free(m);
        }
    }
    for (int i = 0; i < 3000; i++) {
        char g[600];
        size_t l = rnd(sizeof(g));
        for (size_t k = 0; k < l; k++) g[k] = (char)(rnd(4) ? rnd_range(32, 126) : rnd(256));
        fuzz_one(g, l, YML_KEEP_FULL);
    }
    {
        static const char *const nasty[] = {
            "", "\n", ":", " : ", "Preset", "Preset 1", "Preset 1 :", "Channel 99999999999 :\nLevel : 1",
            "Preset 1 :\n Channel 1 :\n  Level : 99999999999999999999\n  Pitch : 1e5\n  Pan : -\n  PitchCV : \n  PitchCV : 1A\n  PitchCV : 9Z 1\n  Machine : TRX-BD\n  MachinePTCH : 300\n  MachinePTCHMod : cv a 0.5\n",
            "Channel 1 :\nZone 9 :\nZone 1 :\nChannel 0 :\nx\n",
        };
        for (unsigned i = 0; i < sizeof(nasty) / sizeof(nasty[0]); i++) fuzz_one(nasty[i], strlen(nasty[i]), YML_KEEP_FULL);
    }
    {
        char longline[2000];
        memset(longline, 'a', sizeof(longline));
        memcpy(longline, "Preset 1 :\n  ", 13);
        longline[600] = ':';
        fuzz_one(longline, sizeof(longline), YML_KEEP_FULL);
    }
    printf("  %ld fuzz runs\n", fuzz_runs);

    test_samples();
    test_sampler();
    test_pair_keys();
    test_machine_play();
    test_xfade();
    test_tallent();
    test_timelock();
    test_grain();
    test_mixfx();
    test_reverb_unit();
    test_reverb_edits();
    if (getenv("YML_EXACT")) test_exact(getenv("YML_EXACT"));
    test_wav();
    test_wav_write();
    test_ops();
    test_zone_pair();
    test_ssetup();

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures != 0;
}
