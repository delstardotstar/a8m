/*
 * Writes the "A8M EXERCISE" folder: every voice on its own channel with every
 * parameter on a CV (A pitch, B timbre/mod, C envelope/level/other), plus
 * presets for stereo pairs with offsets, START DELAY, the Delay's CV CLOCK,
 * the three reverbs, Grain and Time Lock. Round-trips each file through
 * yml_parse and checks every voice appears.
 * usage: exercise OUTDIR   (tools/exercise.sh builds and runs it; EXERCISE_TRIG=1
 * makes every channel fire when its preset is selected, for emulator runs)
 */
#include "presetkit.h"
#include "mix_fx.h"
#include "r70.h"
#include "r80.h"
#include "grains.h"
#include "timelock.h"

enum { S_SINE = 1, S_L, S_R };
static const char *const smp_names[] = { "", "Sine.wav", "160-09L.wav", "160-09R.wav" };
static const char *name_of(unsigned smp) { return smp <= S_R ? smp_names[smp] : ""; }

#define REL(l) (-3 + ((l) - 'A'))      /* mix and Grain routes: CV A..C of the channel */
#define A CV_REL_A
#define B CV_REL_B
#define C CV_REL_C

static struct preset all[16];
static int count;
static uint8_t seen[VOICE_COUNT];

static struct mix_settings *mix(void)
{
    static const struct mix_comp_set comp_default = { 0, 20, 4, 3, 0, 0, MIX_PERCENT_MAX, MIX_KEY_SELF };
    struct mix_settings *x = (struct mix_settings *)p.mixfx;

    if (x->format != MIX_FORMAT) {
        memset(x, 0, sizeof(*x));
        x->format = MIX_FORMAT;
        for (int c = 0; c < MIX_CHANNELS; c++) {
            x->comp[c] = comp_default;
        }
        x->master = comp_default;
        x->reverb_decay = 25; x->reverb_damp = 20; x->reverb_level = 80;
        x->delay_division = 5; x->delay_feedback = 45; x->delay_low = 3;
        x->delay_high = 2; x->delay_level = 80; x->delay_tempo = MIX_TEMPO_BPM + 120 - MIX_BPM_MIN;
        x->reverb_algo = 1; x->chorus_rate = 40; x->chorus_depth = 50;
        for (int n = 0; n < MIX_BUS_ROUTES; n++) {
            x->bus_route[n].source = TA_SRC_OFF;
        }
        for (int c = 0; c < MIX_CHANNELS; c++) {
            for (int n = 0; n < MIX_SEND_ROUTES; n++) {
                x->send_route[c][n].source = TA_SRC_OFF;
            }
        }
    }
    return x;
}

static void fresh(const char *name)
{
    kit(name);
    for (int c = 0; c < CHANNELS; c++) {
        sampler_defaults(&p.ch[c].s);
    }
    for (int g = 0; g < XFADE_GROUPS; g++) {
        p.xfade_width[g] = XFW_DEFAULT;
    }
    mix();
}

static void smod(int c, int which, int src, int amt)
{
    p.ch[c - 1].s.mod[which].src = (uint8_t)src;
    p.ch[c - 1].s.mod[which].amt = (int16_t)amt;
}

/* A sample channel looping smp. */
static void loop(int c, int smp, int cents)
{
    struct channel_cfg *ch = &p.ch[c - 1];

    ch->machine = 0;
    ch->s.play_mode = PLAY_ONESHOT;
    ch->s.loop_mode = LOOP_NORMAL;
    ch->s.zone[0].smp = (uint16_t)smp;
    ch->pitch = (int16_t)cents;
}

/* The channel-level routes: A pitch; B Bits, Aliasing; C level, pan, attack,
 * release, START DELAY and both sends. */
static void wire_channel(int c)
{
    struct mix_settings *x = mix();

    cv(c, D_PITCH, A, 50);
    cv(c, D_BITS, B, 60);
    smod(c, SM_ALIAS, B, 40);
    cv(c, D_LEVEL, C, 40);
    cv(c, D_PAN, C, 50);
    smod(c, SM_ATTACK, C, 30);
    smod(c, SM_RELEASE, C, 30);
    smod(c, SM_DELAY, C, 20);
    x->reverb_send[c - 1] = 25;
    x->delay_send[c - 1] = 25;
    x->send_route[c - 1][MIX_ROUTE_REVERB_SEND] = (struct mix_route){ REL('C'), 50 };
    x->send_route[c - 1][MIX_ROUTE_DELAY_SEND] = (struct mix_route){ REL('C'), 50 };
}

/* Pitch-tagged controls on A, timbre and mod on B, envelope and level on C. */
static void wire_voice(int c)
{
    const struct voice_desc *v = voices[p.ch[c - 1].machine];
    static const int jack_of[] = { [TAG_PITCH] = A, [TAG_ENV] = C, [TAG_TIMBRE] = B, [TAG_MOD] = B, [TAG_LEVEL] = C };
    static const int amt_of[] = { [TAG_PITCH] = 40, [TAG_ENV] = 50, [TAG_TIMBRE] = 60, [TAG_MOD] = 60, [TAG_LEVEL] = 50 };

    for (unsigned i = 0; i < v->nparams; i++) {
        cv(c, D_CTRL + (int)i, jack_of[v->params[i].tag], amt_of[v->params[i].tag]);
    }
}

static void wire(int c)
{
    if (p.ch[c - 1].machine) {
        wire_voice(c);
    }
    wire_channel(c);
}

static void stereo_pair(int c, int smp)
{
    struct channel_cfg *r = &p.ch[c];

    if (p.ch[c - 1].machine) {
        m(c + 1, p.ch[c - 1].machine);
    } else {
        loop(c + 1, smp, p.ch[c - 1].pitch);
    }
    r->mode = MODE_STEREO;
    p.ch[c - 1].pan = -100;
    r->pan = 100;
}

static void grain_wire(struct grain_value *g)
{
    static const int16_t dense[TA_GRAIN_FIELDS] = { 1, 300, 600, 50, 30, 25, 25, 100, 60, 50, 0, -20 };
    static const struct { int route; char jack; int amt; } r[] = {
        { GRAIN_ROUTE_DETUNE, 'A', 50 }, { GRAIN_ROUTE_SLIDE, 'A', 40 },
        { GRAIN_ROUTE_SIZE, 'B', 50 }, { GRAIN_ROUTE_OVERLAP, 'B', 40 },
        { GRAIN_ROUTE_SPRAY, 'B', 50 }, { GRAIN_ROUTE_SHAPE, 'B', 40 },
        { GRAIN_ROUTE_SCAN, 'C', 50 }, { GRAIN_ROUTE_PAN, 'C', 50 },
    };

    memcpy(g->value, dense, sizeof(dense));
    for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
        g->source[r[i].route] = (int8_t)REL(r[i].jack);
        g->amount[r[i].route] = (int8_t)r[i].amt;
    }
}

static void emit(int slot)
{
    if (getenv("EXERCISE_TRIG")) {
        for (int c = 0; c < CHANNELS; c++) {
            p.ch[c].auto_trig = p.ch[c].mode != MODE_STEREO && (p.ch[c].machine || p.ch[c].s.zone[0].smp);
        }
    }
    save(slot);
    all[count++] = p;
}

static void voice_preset(int slot, const char *name, const int *ids, int n)
{
    fresh(name);
    for (int i = 0; i < n; i++) {
        m(i + 1, ids[i]);
        wire(i + 1);
    }
    emit(slot);
}

static void check(const char *dir)
{
    static char buf[65536];
    int bad = 0;

    for (int i = 0; i < count; i++) {
        char path[512];
        struct preset q;
        FILE *f;
        size_t n;

        snprintf(path, sizeof(path), "%s/prst%03d.yml", dir, i + 1);
        f = fopen(path, "rb");
        n = f ? fread(buf, 1, sizeof(buf), f) : 0;
        if (f) {
            fclose(f);
        }
        if (yml_parse(buf, n, &q, NULL) != YML_OK || strcmp(q.name, all[i].name)) {
            fprintf(stderr, "%s: does not parse\n", path);
            bad = 1;
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            const struct channel_cfg *a = &all[i].ch[c], *b = &q.ch[c];
            int same = a->machine == b->machine && a->mode == b->mode;

            for (int d = 0; d < D_COUNT && same; d++) {
                same = a->mod[d].src == b->mod[d].src && (!a->mod[d].src || a->mod[d].amt == b->mod[d].amt);
            }
            for (int s = 0; s < SM_COUNT && same; s++) {
                same = a->s.mod[s].src == b->s.mod[s].src;
            }
            same = same && !memcmp(a->ofs, b->ofs, sizeof(a->ofs)) && !memcmp(&a->grain, &b->grain, sizeof(a->grain));
            if (!same) {
                fprintf(stderr, "%s: channel %d differs after parse\n", path, c + 1);
                bad = 1;
            }
            if (b->machine) {
                seen[b->machine] = 1;
            }
        }
        if (memcmp(all[i].mixfx, q.mixfx, sizeof(q.mixfx)) || all[i].reverb_unit != q.reverb_unit) {
            fprintf(stderr, "%s: effects differ after parse\n", path);
            bad = 1;
        }
    }
    for (int v = 1; v < VOICE_COUNT; v++) {
        if (!seen[v]) {
            fprintf(stderr, "voice %s is in no preset\n", voices[v]->name);
            bad = 1;
        }
    }
    if (bad) {
        exit(1);
    }
    printf("%d presets parse; all %d voices present\n", count, VOICE_COUNT - 1);
}

int main(int argc, char **argv)
{
    static const int ids[] = {
        TRX_BD, TRX_SD, TRX_XT, TRX_CP, TRX_RS, TRX_CB, TRX_CH, TRX_OH,         /* TRX A */
        TRX_CY, TRX_MA, TRX_CL, TRX_XC, TRX_B2,                                  /* TRX B */
        EFM_BD, EFM_SD, EFM_XT, EFM_CP, EFM_RS, EFM_CB, EFM_HH, EFM_CY,
        PI_BD, PI_SD, PI_MT, PI_ML, PI_MA, PI_RS, PI_RC, PI_CC,                  /* PI A */
        PI_HH, GND_SN, GND_NS, GND_IM,                                           /* PI B + GND */
        SW_SAW, SID_6581, SW_PULSE, SW_ENS, FM_STAT, FM_PAR, FM_DYN, VO_6,       /* MNM A */
        GND_SIN, GND_NOIS,                                                       /* MNM B */
    };
    static const struct { const char *name; int first, n; } groups[] = {
        { "EX TRX A", 0, 8 }, { "EX TRX B", 8, 5 }, { "EX EFM", 13, 8 }, { "EX PI A", 21, 8 },
        { "EX PI B GND", 29, 4 }, { "EX MNM A", 33, 8 }, { "EX MNM B", 41, 2 },
    };
    struct mix_settings *x;
    int slot = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: exercise OUTDIR\n");
        return 2;
    }
    outdir = argv[1];
    yml_sample_name = name_of;

    for (unsigned g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        voice_preset(++slot, groups[g].name, ids + groups[g].first, groups[g].n);
    }

    fresh("EX STEREO");
    m(1, EFM_BD); wire(1); stereo_pair(1, 0);
    p.ch[1].ofs[PO_PITCH] = 12;
    p.ch[1].ofs[PO_LEVEL] = -30;
    p.ch[1].ofs[PO_CTRL + control(1, "MOD")] = 20;
    p.ch[1].ofs[PO_CTRL + control(1, "DEC")] = -10;
    loop(3, S_L, 0); wire(3); stereo_pair(3, S_R);
    p.ch[3].ofs[PO_PITCH] = 7;
    p.ch[3].ofs[PO_LEVEL] = -20;
    p.ch[3].ofs[PO_BITS] = -30;
    p.ch[3].ofs[PO_ATTACK] = 10;
    p.ch[3].ofs[PO_RELEASE] = 10;
    emit(++slot);

    fresh("EX HAAS");
    loop(1, S_L, 0); wire(1); stereo_pair(1, S_R);
    p.ch[1].ofs[PO_DELAY] = 150;
    m(3, SW_SAW); wire(3); stereo_pair(3, 0);
    p.ch[3].ofs[PO_DELAY] = 250;
    p.ch[3].ofs[PO_PITCH] = 5;
    loop(5, S_SINE, 0); wire(5);
    m(6, TRX_SD); wire(6);
    p.ch[4].s.start_delay = 100;
    p.ch[5].s.start_delay = 200;
    emit(++slot);

    fresh("EX DELAY CLK");
    x = mix();
    x->delay_tempo = MIX_TEMPO_CV;
    x->delay_clk_src = 0;
    x->delay_clk_ppqn = 4;
    x->delay_division = 6;
    x->delay_feedback = 60;
    m(1, EFM_SD); wire(1); p.ch[0].mod[D_PITCH].src = CV_OFF;
    m(2, TRX_BD); wire(2);
    m(3, TRX_CH); wire(3);
    m(4, SW_SAW); wire(4);
    for (int c = 0; c < 4; c++) {
        x->delay_send[c] = 60;
    }
    x->bus_route[MIX_ROUTE_DELAY_FEEDBACK] = (struct mix_route){ 21, 50 };
    x->bus_route[MIX_ROUTE_DELAY_LEVEL] = (struct mix_route){ 22, 50 };
    x->bus_route[MIX_ROUTE_DELAY_TIME] = (struct mix_route){ 23, 100 };
    emit(++slot);

    fresh("EX REV DEP5");
    x = mix();
    x->reverb_algo = 3;
    m(1, TRX_SD); wire(1);
    m(2, EFM_CP); wire(2);
    m(3, PI_MT); wire(3);
    m(4, SW_PULSE); wire(4);
    for (int c = 0; c < 4; c++) {
        x->reverb_send[c] = 60;
    }
    x->bus_route[MIX_ROUTE_REVERB_DECAY] = (struct mix_route){ 21, 50 };
    x->bus_route[MIX_ROUTE_REVERB_DAMP] = (struct mix_route){ 22, 50 };
    x->bus_route[MIX_ROUTE_REVERB_LEVEL] = (struct mix_route){ 23, 40 };
    x->bus_route[MIX_ROUTE_CHORUS_RATE] = (struct mix_route){ 21, 30 };
    x->bus_route[MIX_ROUTE_CHORUS_DEPTH] = (struct mix_route){ 22, 30 };
    emit(++slot);

    for (int unit = 0; unit < 2; unit++) {
        fresh(unit ? "EX REV 80" : "EX REV 70");
        x = mix();
        p.reverb_unit = unit ? REVERB_80 : REVERB_70;
        p.reverb_program = (uint8_t)(unit ? r80_find(10) ? 10 : r80_find(1)->number : r70_find(10) ? 10 : r70_find(1)->number);
        for (int e = 0; e < REVERB_EDITS; e++) {
            p.reverb_edit[e] = -1;
            p.reverb_route[e].source = TA_SRC_OFF;
        }
        for (int e = 0; e < 3; e++) {
            p.reverb_route[e] = (struct reverb_route){ (signed char)(21 + e), 50 };
        }
        m(1, TRX_SD); wire(1);
        m(2, EFM_CP); wire(2);
        m(3, PI_MT); wire(3);
        m(4, SW_PULSE); wire(4);
        for (int c = 0; c < 4; c++) {
            x->reverb_send[c] = 60;
        }
        emit(++slot);
    }

    fresh("EX GRAIN");
    loop(1, S_SINE, -1200); wire(1); grain_wire(&p.ch[0].grain);
    loop(2, S_L, 0); wire(2); grain_wire(&p.ch[1].grain); stereo_pair(2, S_R);
    p.ch[2].grain = p.ch[1].grain;
    loop(4, S_R, 0); wire(4); grain_wire(&p.ch[3].grain);
    emit(++slot);

    fresh("EX TLOCK");
    loop(1, S_L, 0); wire(1);
    p.ch[0].tl = (struct timelock_value){ 1, TIME_LOCK_TEMPO_CV, 4, 21, 0 };
    loop(2, S_R, 0); wire(2);
    p.ch[1].tl = (struct timelock_value){ 1, TIME_LOCK_TEMPO_CHANNEL_1, 0, TA_SRC_OFF, 0 };
    loop(3, S_L, 0); wire(3);
    p.ch[2].tl = (struct timelock_value){ 1, TIME_LOCK_TEMPO_SPEED, 0, TA_SRC_OFF, 500 };
    loop(4, S_R, 0); wire(4);
    p.ch[3].tl = (struct timelock_value){ 1, TIME_LOCK_TEMPO_OWN, 0, TA_SRC_OFF, 0 };
    emit(++slot);

    check(outdir);
    return 0;
}
