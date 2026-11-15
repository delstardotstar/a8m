#ifndef PRESET_H_INCLUDED
#define PRESET_H_INCLUDED

#include <stdint.h>
#include <string.h>

#define CHANNELS        8
#define CONTROLS        8       /* mdkit voices have up to 8 */

/* Modulation destinations of a channel; each takes one CV with an amount. */
enum dest {
    D_PITCH,        /* cents, -2400..2400 (sampler SPITCH_*); 0.50 = 1 V/oct (stock) */
    D_LEVEL,        /* tenths of a dB, LEVEL_MIN (= off)..120 (sampler SLEVEL_*) */
    D_PAN,          /* -100 (L)..100 (R) */
    D_MIX,          /* tenths of a dB, mix output level */
    D_BITS,         /* tenths of a bit, 10..320 (320 = full depth, stock) */
    D_CTRL,         /* + control index: machine controls, 0..127 */
    D_COUNT = D_CTRL + CONTROLS
};

#define LEVEL_MIN   (-600)
#define LEVEL_MAX   120
#define SLEVEL_MIN  (-900)      /* sampler channels: -90 dB (stock) */
#define SLEVEL_MAX  60
#define MIX_MIN     (-900)      /* mix level: -89.9 dB, then off (stock) */
#define SPITCH_MIN  (-9600)
#define SPITCH_MAX  6000
#define BITS_MIN    10
#define BITS_MAX    320
#define AMT_MAX     200         /* hundredths: -2.00..+2.00 */
/* A new route's amount: stock's 1 V/oct for pitch, 1.00 otherwise. */
#define AMT_DEFAULT(d) ((int16_t)((d) == D_PITCH ? 50 : 100))

/* CV sources, as on the A8: Off, the channel's own A/B/C ("relative"),
 * then the 24 inputs 1A..8C ("absolute"). */
enum { CV_OFF, CV_REL_A, CV_REL_B, CV_REL_C, CV_ABS, CV_SRC_COUNT = CV_ABS + 24 };

struct mod {
    uint8_t src;
    uint8_t pad;
    int16_t amt;
};

/* Tallent Edition EQ and filter records (fw/tallent/eq.h has their
 * units). A zone record with override 0 follows its channel's. Records
 * compare by memcmp: no padding, reserved stays 0. */
#define TA_EQ_MODS      7
#define TA_FILTER_MODS  4
#define TA_SRC_OFF      (-4)    /* source: -4 Off, -3..-1 CV A..C, 0..23 1A..8C */
#define TA_MIXFX_BYTES  256     /* MixFX1..16: struct mix_settings (fw/tallent/mix_fx.h), all 0 = defaults */
#define TA_GRAIN_FIELDS 12      /* fw/tallent/grains.h: GRAIN_ON..GRAIN_SKEW */
#define TA_GRAIN_ROUTES 8

struct eq_value {
    uint16_t hpf;
    int16_t gain[3];
    uint16_t frequency[3];
    uint8_t on, override;
    int8_t source[TA_EQ_MODS], amount[TA_EQ_MODS];
    uint8_t q[3], reserved;
};

struct filter_value {
    uint8_t type, resonance, extra, override;
    uint16_t cutoff;
    int8_t source[TA_FILTER_MODS], amount[TA_FILTER_MODS];
    int16_t right;
};

/* Tallent's struct grain_settings: the fields in Grain<c> order, a route
 * for eight of them (GrainCV<c>). */
struct grain_value {
    int16_t value[TA_GRAIN_FIELDS];
    int8_t source[TA_GRAIN_ROUTES], amount[TA_GRAIN_ROUTES];
};

/* Tallent's Time Lock settings of a channel (TimeLock<c>): tempo
 * TIME_LOCK_TEMPO_* (fw/tallent/timelock.h), Speed in tenths of a percent
 * from 100%, the CV clock's source (TA_SRC_*) and pulses a beat (0: 4). A
 * zone's Beats is zone_cfg.beats. */
struct timelock_value {
    uint8_t on, tempo, ppqn;
    int8_t input;
    int16_t speed;
};

static inline void timelock_value_defaults(struct timelock_value *t)
{
    memset(t, 0, sizeof(*t));
    t->input = TA_SRC_OFF;
}

static inline void eq_value_defaults(struct eq_value *v)
{
    memset(v, 0, sizeof(*v));
    v->frequency[0] = 349;
    v->frequency[1] = 677;
    v->frequency[2] = 1076;
    for (int m = 0; m < TA_EQ_MODS; m++) {
        v->source[m] = TA_SRC_OFF;
        v->amount[m] = 100;
    }
}

static inline void filter_value_defaults(struct filter_value *v)
{
    memset(v, 0, sizeof(*v));
    v->cutoff = 677;
    v->extra = 50;
    for (int m = 0; m < TA_FILTER_MODS; m++) {
        v->source[m] = TA_SRC_OFF;
        v->amount[m] = 100;
    }
}

static inline void grain_value_defaults(struct grain_value *g)
{
    static const int16_t initial[TA_GRAIN_FIELDS] = { 0, 600, 400, 20, 0, 0, 0, 50, 100, 100, 0, 0 };

    memcpy(g->value, initial, sizeof(initial));
    for (int r = 0; r < TA_GRAIN_ROUTES; r++) {
        g->source[r] = TA_SRC_OFF;
        g->amount[r] = 100;
    }
}

/* A8 channel modes (manual, Channels chapter): how a channel is triggered. */
enum {
    MODE_MASTER,    /* its own gate */
    MODE_LINK,      /* with the nearest Master/Cycle channel above */
    MODE_STEREO,    /* Stereo/Right: like Link, and plays the channel above's
                     * parameters except pan */
    MODE_CYCLE,     /* takes turns with the Master above and its other Cycles */
    MODE_COUNT
};

/* Sampler (stock keys, stock units). A channel plays samples when its
 * machine is Off. Positions are frames; POS_END = the sample's end. */
#define ZONES       8
#define POS_END     0x7fffffffu
#define LOOP_MIN    128u        /* 1/32 frame: 4 frames */
#define ENV_MAX     990000u     /* 0.1 ms: 99 s */

struct zone_cfg {
    uint32_t start, end, loop_start;
    uint32_t loop_len;          /* 1/32 frame, POS_END */
    int16_t pitch;              /* cents, -9600..6000 */
    int16_t level;              /* 0.1 dB, -900..60 */
    int16_t minv;               /* 0.01 V, -500..500: lowest CV selecting it */
    uint8_t side;               /* plane of a stereo sample (Stereo/Right: the other) */
    uint8_t beats;              /* Time Lock: the loop's beats, 0 Auto (Tallent) */
    uint16_t smp;               /* samples[] index + 1 (the loaded folder's), 0 = none */
    struct eq_value eq;         /* the zone's own when override (Tallent) */
    struct filter_value filter;
};

/* Sampler CV routes besides PitchCV/BitsMod/PanMod/MixMod. */
enum smod {
    SM_LINFM, SM_EXPFM, SM_PHASE, SM_PMINDEX, SM_ALIAS, SM_LINAM, SM_EXPAM,
    SM_ATTACK, SM_RELEASE, SM_SSTART, SM_SEND, SM_LSTART, SM_LLEN, SM_DELAY, SM_COUNT
};

/* START DELAY (ours): 0.1 ms, 0..SDELAY_MAX; the UI steps 0.1 ms to 10 ms,
 * 1 ms to 100 ms, then 10 ms. Its CV moves it 100 ms/V at 1.00. */
#define SDELAY_MAX  5000
#define SDELAY_STEPS 230

static inline int sdelay_index(int t)
{
    return t < 100 ? t : t < 1000 ? 90 + t / 10 : 180 + t / 100;
}

static inline int sdelay_time(int i)
{
    return i < 100 ? i : i < 190 ? (i - 90) * 10 : (i - 180) * 100;
}

/* Envelope times (stock resolution) as a step index: 0.1 ms to 9.9 ms,
 * 1 ms to 99 ms, 10 ms to 0.99 s, 0.1 s to 9.9 s, then 1 s to 99 s. */
#define ENV_STEPS   459

static inline int env_index(uint32_t t)
{
    return t < 100 ? (int)t : t < 1000 ? 90 + (int)(t / 10) : t < 10000 ? 180 + (int)(t / 100) :
           t < 100000 ? 270 + (int)(t / 1000) : 360 + (int)(t / 10000);
}

static inline uint32_t env_time(int i)
{
    return i < 100 ? (uint32_t)i : i < 190 ? (uint32_t)(i - 90) * 10 : i < 280 ? (uint32_t)(i - 180) * 100 :
           i < 370 ? (uint32_t)(i - 270) * 1000 : (uint32_t)(i - 360) * 10000;
}

enum { PLAY_GATED, PLAY_ONESHOT };   /* default: PLAY_DEFAULT in voices.h */
/* PMSource (stock's numbers): channels 1-8, the sampling inputs, a CV
 * (the PhaseCV route). */
enum { PM_CH1, PM_LEFT = CHANNELS, PM_RIGHT, PM_CV, PM_SRC_COUNT };
enum { LOOP_OFF, LOOP_NORMAL, LOOP_GATED };
enum { ZSEL_GATE, ZSEL_CONT, ZSEL_RANDOM, ZSEL_ADVANCE };

struct sampler_cfg {
    uint32_t attack, release;   /* 0.1 ms */
    uint8_t play_mode, loop_mode, loop_is_end, reverse;
    uint8_t attack_from_current, splice, zones_rt, mixmod_fader;
    uint8_t linam_ext_env, aliasing, pm_source;
    uint8_t xfade;              /* 0 none, 1..4 = A..D */
    uint8_t zones_cv;           /* CV source */
    uint8_t rev_cv;             /* CV source: flips the direction while high (ours) */
    int16_t pm_index;           /* hundredths, 0..100 */
    uint16_t start_delay;       /* 0.1 ms (ours) */
    struct mod mod[SM_COUNT];
    struct zone_cfg zone[ZONES];
};

/* A Stereo/Right channel's offsets from its master (ours): it plays
 * clamp(master + offset) in the master's units (envelope times in steps
 * of env_index, START DELAY in 0.1 ms, EQ gains in the EQ's units). */
enum pofs {
    PO_PITCH, PO_LEVEL, PO_MIX, PO_BITS, PO_ATTACK, PO_RELEASE, PO_ALIAS, PO_PMINDEX, PO_DELAY,
    PO_EQGAIN, PO_CTRL = PO_EQGAIN + 3, PO_COUNT = PO_CTRL + CONTROLS
};

struct channel_cfg {
    uint8_t machine;            /* voices[] index, 0 = off */
    uint8_t mix_on;
    uint8_t mode;
    uint8_t ctrl[CONTROLS];
    uint8_t auto_trig;          /* Trigger On Select: fires when its preset is selected */
    int16_t pitch, level, pan, mix, bits;
    struct mod mod[D_COUNT];
    struct sampler_cfg s;
    struct eq_value eq;
    struct filter_value filter;
    struct grain_value grain;
    struct timelock_value tl;
    int16_t ofs[PO_COUNT];      /* Stereo/Right only */
};

static inline void channel_fx_defaults(struct channel_cfg *c)
{
    eq_value_defaults(&c->eq);
    filter_value_defaults(&c->filter);
    grain_value_defaults(&c->grain);
    timelock_value_defaults(&c->tl);
}

/* Stock's defaults (absent keys). */
static inline void zone_defaults(struct zone_cfg *z)
{
    memset(z, 0, sizeof(*z));
    z->end = POS_END;
    z->loop_len = POS_END;
}

static inline void sampler_defaults(struct sampler_cfg *s)
{
    memset(s, 0, sizeof(*s));
    s->release = 1000;
    for (int i = 0; i < SM_COUNT; i++) {
        s->mod[i].amt = 100;
    }
    for (int z = 0; z < ZONES; z++) {
        zone_defaults(&s->zone[z]);
    }
}

#define PRESET_MAGIC    0x54535250u     /* "PRST" */
#define PRESET_VERSION  2
#define PRESET_NAME_LEN 12
#define PRESET_SLOTS    999     /* prst001.yml..prst999.yml */

#define REVERB_70       70
#define REVERB_80       80
#define REVERB_EDITS    32

#define XFADE_GROUPS    4
#define XFW_MIN         1       /* crossfade width, 0.01 V */
#define XFW_MAX         1000
#define XFW_DEFAULT     100     /* stock: 1.0 V */

struct preset {
    uint32_t magic;
    uint16_t version;
    uint8_t d2cv;               /* 0 = off, else 1 + input index (1A..8C) */
    uint8_t pad;
    char name[PRESET_NAME_LEN + 1];
    uint8_t xfade_cv[XFADE_GROUPS];     /* as d2cv */
    int16_t xfade_width[XFADE_GROUPS];  /* 0.01 V */
    struct channel_cfg ch[CHANNELS];
    uint8_t mixfx[TA_MIXFX_BYTES];
    /* The mix reverb's unit: 0 the DEP-5's (mixfx's algorithm), REVERB_70
     * or REVERB_80 that unit's factory program reverb_program
     * (fw/src/r70.h, r80.h). */
    uint8_t reverb_unit, reverb_program;
    /* The Reverb page's edits of that program (the unit's edit slots,
     * r70_edit / r80_edit): -1 the program's own value. Picking a program
     * clears them. Their CV routes as the bus routes (source TA_SRC_OFF or
     * 0..23 = 1A..8C, amount -100..100: at 100 5 V crosses the range). */
    int16_t reverb_edit[REVERB_EDITS];
    struct reverb_route { signed char source, amount; } reverb_route[REVERB_EDITS];
};

/* The live preset: the UI writes it, the audio engine reads it. */
extern struct preset preset;
extern int preset_number;       /* its slot in the folder, 1-based */

void preset_init(struct preset *p);
/* Channel whose parameters ch plays with (itself unless Stereo/Right). */
int preset_param_channel(int ch);
/* Any offset set on ch (a Stereo/Right channel's). */
int preset_has_offsets(const struct channel_cfg *c);
/* Offset k's value range (the master's field) on a machine or sample
 * channel, and the value a Stereo/Right channel plays: master value v
 * plus offset o, clamped (o = 0: v as it is). */
void preset_offset_range(int k, int machine, int *lo, int *hi);
int preset_offset_apply(int k, int machine, int v, int o);
void preset_set_machine(struct channel_cfg *c, int machine);
/* Removes the zones playing sample smp (samples[] index + 1), closing the
 * gaps; the number removed. */
int preset_drop_sample(struct preset *p, unsigned smp);

#endif
