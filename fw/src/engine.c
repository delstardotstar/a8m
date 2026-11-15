/*
 * Voices render at mdkit's 44.1 kHz; each channel resamples to 48 kHz with
 * a 4-point Hermite interpolator, whose step also carries the channel's
 * pitch (so PITCH and its CV transpose the whole voice, 1 V/oct at 1.00),
 * except for voices that take the pitch themselves (VF_PITCH_IN);
 * with Aliasing, the sample kernels instead. The channel envelope follows.
 * A voice that has been silent for a while after its last trigger is put
 * to sleep (not rendered) until the next one.
 */
#include "engine.h"
#include "cv.h"
#include "diag.h"
#include "fx.h"
#include "grains.h"
#include "hw.h"
#include "interp.h"
#include "panel.h"
#include "pitch.h"
#include "prof.h"
#include "samples.h"
#include "systime.h"
#include "tlock.h"
#include "voices.h"

#include <arm_neon.h>
#include <stddef.h>
#include <string.h>

#define VOICE_RATE      44100.0f
#define MAX_STEP        (4u << 16)      /* +24 semitones */
#define MPITCH_MAX      6000            /* VF_PITCH_IN voices: up to C9 */
#define SRC_MAX         (AUDIO_BLOCK * 4 + 4)
#define QUIET_LEVEL     1e-5f
#define QUIET_BLOCKS    (96 * BLOCKS_PER_32)    /* ~64 ms */
#if !defined(OVERLOAD_US) && defined(PROFILE_VOICE)
#define OVERLOAD_US     0xffffffffu     /* profiles: the emulator's timer runs on host time */
#elif !defined(OVERLOAD_US)
#define OVERLOAD_US     600u            /* per 32 frames (667 us) */
#endif
#define OVERLOAD_BLOCKS 16              /* 32-frame windows in a row */
#define OVERLOAD_HOLD   (AUDIO_RATE / AUDIO_BLOCK)  /* triggers ignored ~1 s after a trip */
#define FADE_BLOCKS     (8 * BLOCKS_PER_32)     /* latch release: ~5 ms to silence */
#define DEFER_BLOCKS    (64 * BLOCKS_PER_32)    /* a trigger waits this long for its machine */
#define PCV_ON          1.7f
#define PCV_OFF         1.5f
#define MHIST           8               /* machine frames kept from the last block */
#define MDELAY          2               /* Aliasing reads this many frames later than Hermite */
#define FX_TAIL_BLOCKS  (16 * BLOCKS_PER_32)    /* quiet blocks a silent channel's filter/EQ still runs */
#define HUSH_BLOCKS     (8 * BLOCKS_PER_32)     /* engine_hush: ~5 ms fade to silence */

struct rt {
    uint8_t machine;            /* voice in *state */
    uint8_t asleep;
    volatile uint8_t ready_machine;     /* voice in *spare, valid when ready */
    volatile uint8_t ready;
    unsigned salt, ready_salt;          /* mdkit_noise_salt of *state, *spare */
    uint8_t *state, *spare;
    uint8_t applied[CONTROLS];
    uint16_t quiet;
    uint8_t fade;                       /* blocks left of a release fade */
    uint32_t frac;                      /* Q16 position between hist[MHIST - 3] and hist[MHIST - 2] */
    uint32_t step;                      /* last block's, ramped from (pitch CV) */
    float gain;                         /* last block's, ramped from (level CV) */
    float hist[MHIST];
    /* The last block's step, level gain and Bits quantiser and what they
     * came from (each a few VFP ops and fast_exp2s; CV rarely moves them). */
    uint8_t memo;                       /* bit per valid entry below */
    int step_key, level_key, bits_key;
    uint32_t step_val;
    float level_val, bits_q, bits_iq;
    /* Chunked voices (TRX, PI: 32-sample internal blocks, triggers at
     * their boundaries): frames rendered ahead, the voice's position in
     * its internal block; chunked: one rendered this block, need frames. */
    uint8_t fifo_n, phase, chunked, need;
    uint16_t cost;
    float fifo[SRC_MAX + 1];
    /* Controls (channel_render), worked out on the channel's control
     * blocks (ctl_block) and triggers; ctl_ok: they are valid. */
    uint8_t ctl_ok;
    int ctl_cents, ctl_level, ctl_bits;
    float ctl_at_k, ctl_rl_k;
};

/* A sample voice: one per output channel, used while its machine is Off. */
enum { ENV_ATTACK, ENV_RELEASE };
struct svoice {
    uint8_t on, stage, zone, looping, rev, adv, tail;
    float last;                 /* output before the envelope; held in the tail */
    int16_t smp;
    const int32_t *x;
    int64_t pos;                /* Q32.32 frames, where the last frame was read */
    int64_t start, end, lstart, lend;
    int64_t pmax;               /* last frame of the plane */
    float env;
    float fb;                   /* self phase mod: last output frame */
    uint8_t splice;             /* SpliceSmoothing: loop wraps and retriggers are smoothed */
    float sd;                   /* splice offset, added to the output and decaying */
    float pmod[4];              /* SampleStart/End, LoopStart/Length mods: V x amount */
    uint16_t k;                 /* interpolation kernel of the last frame */
    uint32_t rate;              /* rate_lp's file rate */
    int32_t rate_lp;
    uint32_t at_ms, rl_ms;      /* env_attack/env_release memo */
    float at_c, rl_c;
    int64_t *trace;             /* grains: each frame's position, while rendering */
    uint8_t unread;             /* grains replace its output: positions only */
};

static int64_t sv_limit(const struct svoice *v);
/* Two states per channel: the main loop initializes the spare (PI voices
 * zero ~271 KB, far longer than a block) and the IRQ swaps it in. */
static uint8_t states[CHANNELS][2][VOICE_STATE_MAX] __attribute__((aligned(16)));
static struct rt rt[CHANNELS];
static struct svoice sv[CHANNELS];
/* sampler_render's controls, as rt's ctl_. */
static struct sctl {
    uint8_t ok, g_ok;
    int level, bits;
    float am, at_k, rl_k;
    int g_db;                   /* g = the gain of g_db at am */
    float g_am, g;
} sctl[CHANNELS];
static uint8_t revcv[CHANNELS];
volatile uint8_t engine_buttons;
volatile uint8_t engine_virtual_buttons;
static volatile uint8_t ui_trig[CHANNELS], auto_trig[CHANNELS], latch_req[CHANNELS];
static volatile uint8_t gate_delay;     /* frames */
static uint16_t gate_wait[CHANNELS];    /* 1 + frames until a delayed gate fires, 0 none */
static uint8_t deferred, defer_age[CHANNELS];
/* START DELAY: each channel's triggers waiting, in order (a full queue
 * drops new ones), as the frame each fires at, its source and delay. */
#define SD_QUEUE        16
static struct sd_queue {
    uint32_t at[SD_QUEUE];
    uint16_t delay[SD_QUEUE];
    uint8_t src[SD_QUEUE];
    uint8_t head, n;
} sdq[CHANNELS];
static uint32_t frame_now;              /* frames before this block */
/* The playing note's start delay in frames: its gate (or button) is read
 * that much later, so a gated release comes as late as its start. */
static uint16_t note_delay[CHANNELS];
#define HELD_BLOCKS     2048            /* > SDELAY_MAX's frames / AUDIO_BLOCK + 1 */
static struct held_hist {
    uint32_t gate[CHANNELS];
    uint8_t buttons, latched;
} held_hist[HELD_BLOCKS];
static uint32_t block_now;
/* Stereo/Right channels with offsets: their parameters (the master's with
 * the offsets) for this block; vslot: where modulated() leaves values. */
static struct channel_cfg eff[CHANNELS];
static uint8_t eff_on;
static int vslot = -1;
static uint8_t last_gates;
/* Frames of this block (bit per frame) each gate input is high. */
static uint32_t gate_held[CHANNELS];
_Static_assert(AUDIO_BLOCK <= 32, "gate_held is a frame mask");
_Static_assert(SDELAY_MAX * (AUDIO_RATE / 10000) / AUDIO_BLOCK + 1 < HELD_BLOCKS, "held_hist too short");
volatile uint8_t engine_preset_cv_in[PCV_COUNT];
static unsigned overload_run, overload_hold;
/* engine_hush: requested (main loop), blocks of fade left, and done (IRQ:
 * silent, voices stopped, triggers held in hush_fire). */
static volatile uint8_t hush_req, hushed;
static uint8_t hush_fade, hush_fire;
static uint8_t mix_live;
static float mix_gl[CHANNELS], mix_gr[CHANNELS];    /* last block's mix gains */
static float mix_g[CHANNELS];                       /* and mix levels (Post sends) */
static uint8_t mix_memo;                            /* mix_val[ch] = the gain of mix_key[ch] dB/10 */
static int mix_key[CHANNELS];
static float mix_val[CHANNELS];
static int32_t sends[AUDIO_BLOCK][CHANNELS] __attribute__((aligned(16)));
/* Channel outputs, this block's and the last (phase mod sources). */
static float chout[2][CHANNELS][AUDIO_BLOCK + 4] __attribute__((aligned(16)));
/* Phase mod sources' signal before the level (stock: a source's Level
 * doesn't change the depth, measured), this block's and the last. */
static float chmod[2][CHANNELS][AUDIO_BLOCK + 4] __attribute__((aligned(16)));
static unsigned chout_cur;
static struct grain_state grain_st[CHANNELS];
static uint8_t grain_live, grain_right_ok;
static float grain_right[CHANNELS][AUDIO_BLOCK] __attribute__((aligned(16)));
/* Time Lock on a pair: what the master's lock did at each frame, for its
 * Stereo/Right channel (gain TL_NONE: not mixed; write INT64_MIN: none). */
#define TL_NONE 0xffffffffu
static uint8_t tl_right_ok;
static uint32_t tl_rgain[CHANNELS][AUDIO_BLOCK];
static int64_t tl_rsum[CHANNELS][AUDIO_BLOCK], tl_rwrite[CHANNELS][AUDIO_BLOCK];
static uint8_t fx_tail[CHANNELS];
static unsigned chunk_cost;     /* this block's internal blocks, by worst_us */
#ifdef CHUNK_TRACE
uint16_t chunk_trace[4096];
#endif
#ifdef TRIG_TRACE
uint32_t trig_trace[2][FAMILY_COUNT];   /* chunked voices' triggers, and those landing an internal block later */
uint32_t chunk_hist[17], chunk_span[33];    /* internal blocks per block, per 32 frames */
static unsigned chunk_n, chunk_prev;
#endif
static uint8_t chout_done, pm_srcs;
static const int32_t (*sample_in)[2];
volatile struct engine_status engine_status;
volatile int16_t engine_value[CHANNELS][D_COUNT];
volatile uint8_t engine_zone_lock[CHANNELS];
volatile int engine_scope_ch;
volatile int8_t engine_scope_min[SCOPE_N], engine_scope_max[SCOPE_N];
volatile unsigned engine_scope_head;

static int8_t scope_q(float s)
{
    return (int8_t)(s > 127.0f ? 127 : s < -127.0f ? -127 : (int)s);
}

static void scope_capture(const float *buf, unsigned frames)
{
    const float k = 127.0f;

    for (unsigned b = 0; b + 16 <= frames; b += 16) {
        float32x4_t x0 = vld1q_f32(buf + b), x1 = vld1q_f32(buf + b + 4), x2 = vld1q_f32(buf + b + 8),
                    x3 = vld1q_f32(buf + b + 12);
        float32x4_t mn = vminq_f32(vminq_f32(x0, x1), vminq_f32(x2, x3));
        float32x4_t mx = vmaxq_f32(vmaxq_f32(x0, x1), vmaxq_f32(x2, x3));
        float32x2_t l2 = vpmin_f32(vget_low_f32(mn), vget_high_f32(mn));
        float32x2_t h2 = vpmax_f32(vget_low_f32(mx), vget_high_f32(mx));
        float lo = vget_lane_f32(vpmin_f32(l2, l2), 0), hi = vget_lane_f32(vpmax_f32(h2, h2), 0);
        unsigned h = engine_scope_head;
        engine_scope_min[h] = scope_q(lo * k);
        engine_scope_max[h] = scope_q(hi * k);
        engine_scope_head = (h + 1) % SCOPE_N;
    }
}

/* 2^x, |x| < 32; ~1e-7 relative error. */
float fast_exp2(float x)
{
    int i = (int)x - (x < 0 && x != (int)x);
    float f = x - (float)i;
    float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f +
              f * (0.0096181f + f * (0.0013333f + f * 0.0001540f)))));
    union { float f; uint32_t u; } v = { p };

    v.u += (uint32_t)i << 23;
    return v.f;
}

/* A machine pair's right voice gets its own noise (mdkit_noise_salt);
 * other voices 0, upstream's. */
static unsigned pair_salt(int ch, int pc, int machine)
{
    return pc != ch && machine ? (0x9e3779u * (unsigned)ch) & 0xffffffu : 0u;
}

void engine_service(void)
{
    if (!rt[0].state) {
        interp_init();
        for (int ch = 0; ch < CHANNELS; ch++) {
            rt[ch].state = states[ch][0];
            rt[ch].spare = states[ch][1];
        }
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        struct rt *r = &rt[ch];
        int pc = preset_param_channel(ch), m = preset.ch[pc].machine;
        unsigned salt = pair_salt(ch, pc, m);

        if (r->ready || (m == r->machine && salt == r->salt) || m >= VOICE_COUNT) {
            continue;
        }
        if (voices[m]) {
            unsigned keep = mdkit_noise_salt;
            mdkit_noise_salt = salt;
            voices[m]->ops->init(r->spare);
            mdkit_noise_salt = keep;
        }
        r->ready_salt = salt;
        r->ready_machine = (uint8_t)m;
        __asm__ volatile("dmb" ::: "memory");
        r->ready = 1;
    }
    fx_poll();
}

static int is_master(int ch)
{
    return ch == 0 || preset.ch[ch].mode == MODE_MASTER;
}

/* Channels a trigger on ch's gate fires, by the A8 channel modes: only a
 * Master listens to its gate; it fires itself or, taking turns, one of the
 * Cycle channels below it (up to the next Master), plus the Link and
 * Stereo/Right channels directly below whichever fires. */
static uint8_t gate_group(int ch)
{
    static uint8_t cycle_pos[CHANNELS];
    int heads[CHANNELS], n = 0;
    uint8_t mask;

    if (!is_master(ch)) {
        return 0;
    }
    heads[n++] = ch;
    for (int c = ch + 1; c < CHANNELS && !is_master(c); c++) {
        if (preset.ch[c].mode == MODE_CYCLE) {
            heads[n++] = c;
        }
    }
    int head = heads[cycle_pos[ch] % n];
    cycle_pos[ch] = (uint8_t)((cycle_pos[ch] + 1) % n);
    mask = (uint8_t)(1u << head);
    for (int c = head + 1; c < CHANNELS; c++) {
        int m = preset.ch[c].mode;
        if (m != MODE_LINK && m != MODE_STEREO) {
            break;
        }
        mask |= (uint8_t)(1u << c);
    }
    return mask;
}

void engine_trigger(int ch)
{
    if (ch >= 0 && ch < CHANNELS) {
        ui_trig[ch] = 1;
    }
}

void engine_auto_trigger(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (preset.ch[ch].auto_trig) {
            auto_trig[ch] = 1;
        }
    }
}

void engine_latch(int ch)
{
    if (ch >= 0 && ch < CHANNELS) {
        latch_req[ch] = 1;
    }
}

void engine_set_gate_delay(int tenths)
{
    tenths = tenths < 0 ? 0 : tenths > 50 ? 50 : tenths;
    gate_delay = (uint8_t)((tenths * AUDIO_RATE / 1000 + 5) / 10);
}

/* Inc/Dec/Reset preset inputs: armed at <= 1.5 V, a rise to >= 1.7 V counts. */
static void preset_cv_scan(void)
{
    static uint8_t armed;

    for (int i = 0; i < PCV_COUNT; i++) {
        int in = engine_preset_cv_in[i];
        uint8_t b = (uint8_t)(1u << i);

        if (!in || in > CV_INPUTS) {
            armed &= (uint8_t)~b;
            continue;
        }
        float v = cv_volts[in - 1];
        if (v <= PCV_OFF) {
            armed |= b;
        } else if ((armed & b) && v >= PCV_ON) {
            armed &= (uint8_t)~b;
            engine_status.preset_cv[i]++;
            engine_status.preset_cv_ticks = time_ticks();
        }
    }
}

void engine_diag(int ch, uint32_t w[4])
{
    const struct svoice *v = &sv[ch & 7];

    w[0] = (uint32_t)v->x;
    w[1] = (uint32_t)(v->pos >> 32);
    w[2] = (uint32_t)(sv_limit(v) >> 32);
    w[3] = (uint32_t)v->on | (uint32_t)v->tail << 4 | (uint32_t)v->rev << 8 | (uint32_t)v->looping << 12 |
           (uint32_t)(uint16_t)v->smp << 16;
}

float engine_cv(int src, int ch)
{
    if (src >= CV_REL_A && src <= CV_REL_C) {
        return cv_volts[ch * 3 + (src - CV_REL_A)];
    }
    if (src >= CV_ABS && src < CV_SRC_COUNT) {
        return cv_volts[src - CV_ABS];
    }
    return 0.0f;
}

/* Units per volt at amount 1.00, by destination. */
static float per_volt(int d)
{
    switch (d) {
    /* Stock's scales at amount 1.00, so yml amounts mean the same. */
    case D_PITCH: return 2400.0f;       /* cents: 2 oct/V (0.50 = 1 V/oct) */
    case D_LEVEL: return 120.0f;        /* 12 dB/V (ours, MLevelMod) */
    case D_PAN:   return 40.0f;         /* 40%/V */
    case D_MIX:   return 100.0f;        /* 10 dB/V */
    case D_BITS:  return 32.0f;         /* 3.2 bits/V */
    default:      return 127.0f / 5.0f; /* controls: full range over 5 V */
    }
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int modulated(const struct channel_cfg *c, int ch, int d, int base, int lo, int hi)
{
    const struct mod *m = &c->mod[d];
    int v = base;

    if (m->src != CV_OFF) {
        float dv = engine_cv(m->src, ch) * per_volt(d) * (float)m->amt * 0.01f;
        v += (int)(dv + (dv < 0 ? -0.5f : 0.5f));
    }
    v = clampi(v, lo, hi);
    engine_value[vslot >= 0 ? vslot : ch][d] = (int16_t)v;
    return v;
}

/* Control-rate work (modulated parameters, mix gains) runs once per 32
 * frames, as on 32-frame blocks: channels 1-4 on one 16-frame block,
 * 5-8 on the next, and on a trigger. */
static int ctl_block(int ch)
{
    return (block_now + (unsigned)ch * BLOCKS_PER_32 / CHANNELS) % BLOCKS_PER_32 == 0;
}

static float db_gain(int tenths)
{
    return tenths <= LEVEL_MIN ? 0.0f : fast_exp2((float)tenths * (0.1f / 6.0205999f));
}

static void wake(struct rt *r)
{
    r->asleep = 0;
    r->quiet = 0;
}

static void sleep_voice(struct rt *r)
{
    r->asleep = 1;
    r->frac = 0;
    r->step = 0;
    r->gain = -1.0f;
    memset(r->hist, 0, sizeof(r->hist));
}

/* Largest |x| over n floats (n > 0); the buffer must have room to round n
 * up to a multiple of 4 (the extra lanes are zeroed). */
static float peak_abs(float *x, unsigned n)
{
    float32x4_t m = vdupq_n_f32(0.0f);
    unsigned n4 = (n + 3u) & ~3u;

    for (unsigned i = n; i < n4; i++) {
        x[i] = 0.0f;
    }
    for (unsigned i = 0; i < n4; i += 4) {
        m = vmaxq_f32(m, vabsq_f32(vld1q_f32(x + i)));
    }
    float32x2_t h = vpmax_f32(vget_low_f32(m), vget_high_f32(m));
    return vget_lane_f32(vpmax_f32(h, h), 0);
}

/* 4-point Hermite, 4 outputs at a time. Output f reads the window
 * x[m .. m + 3], m = (frac + f * step) >> 16, at t = the low 16 bits. */
static void resample(const float *x, uint32_t frac, uint32_t step, float *out, unsigned frames)
{
    const float32x4_t half = vdupq_n_f32(0.5f), one5 = vdupq_n_f32(1.5f);
    const float32x4_t two5 = vdupq_n_f32(2.5f), two = vdupq_n_f32(2.0f);
    uint32x4_t p = { frac, frac + step, frac + 2 * step, frac + 3 * step };
    const uint32x4_t step4 = vdupq_n_u32(4 * step), low = vdupq_n_u32(0xffffu);

    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t x0 = vdupq_n_f32(0), x1 = x0, x2 = x0, x3 = x0;
        const float *w;
#define LANE(i) \
        w = x + ((frac + (f + i) * step) >> 16); \
        x0 = vld1q_lane_f32(w, x0, i); \
        x1 = vld1q_lane_f32(w + 1, x1, i); \
        x2 = vld1q_lane_f32(w + 2, x2, i); \
        x3 = vld1q_lane_f32(w + 3, x3, i);
        LANE(0) LANE(1) LANE(2) LANE(3)
#undef LANE
        float32x4_t t = vcvtq_n_f32_u32(vandq_u32(p, low), 16);
        float32x4_t c1 = vmulq_f32(half, vsubq_f32(x2, x0));
        float32x4_t c2 = vsubq_f32(vmlaq_f32(vmlsq_f32(x0, two5, x1), two, x2), vmulq_f32(half, x3));
        float32x4_t c3 = vmlaq_f32(vmulq_f32(half, vsubq_f32(x3, x0)), one5, vsubq_f32(x1, x2));
        float32x4_t y = vmlaq_f32(c2, c3, t);
        y = vmlaq_f32(c1, y, t);
        y = vmlaq_f32(x1, y, t);
        vst1q_f32(out + f, y);
        p = vaddq_u32(p, step4);
    }
}

/* The same at per-frame positions pos[0..frames) (a pitch ramp). */
static void resample_pos(const float *x, const uint32_t *pos, float *out, unsigned frames)
{
    const float32x4_t half = vdupq_n_f32(0.5f), one5 = vdupq_n_f32(1.5f);
    const float32x4_t two5 = vdupq_n_f32(2.5f), two = vdupq_n_f32(2.0f);
    const uint32x4_t low = vdupq_n_u32(0xffffu);

    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t x0 = vdupq_n_f32(0), x1 = x0, x2 = x0, x3 = x0;
        const float *w;
#define LANE(i) \
        w = x + (pos[f + i] >> 16); \
        x0 = vld1q_lane_f32(w, x0, i); \
        x1 = vld1q_lane_f32(w + 1, x1, i); \
        x2 = vld1q_lane_f32(w + 2, x2, i); \
        x3 = vld1q_lane_f32(w + 3, x3, i);
        LANE(0) LANE(1) LANE(2) LANE(3)
#undef LANE
        float32x4_t t = vcvtq_n_f32_u32(vandq_u32(vld1q_u32(pos + f), low), 16);
        float32x4_t c1 = vmulq_f32(half, vsubq_f32(x2, x0));
        float32x4_t c2 = vsubq_f32(vmlaq_f32(vmlsq_f32(x0, two5, x1), two, x2), vmulq_f32(half, x3));
        float32x4_t c3 = vmlaq_f32(vmulq_f32(half, vsubq_f32(x3, x0)), one5, vsubq_f32(x1, x2));
        float32x4_t y = vmlaq_f32(c2, c3, t);
        y = vmlaq_f32(c1, y, t);
        y = vmlaq_f32(x1, y, t);
        vst1q_f32(out + f, y);
    }
}

/* Multiplies buf by a gain going linearly from g0 (exclusive) to g1. */
static void gain_ramp(float *buf, unsigned frames, float g0, float g1)
{
    if (g0 == g1) {
        for (unsigned f = 0; f < frames; f += 4) {
            vst1q_f32(buf + f, vmulq_n_f32(vld1q_f32(buf + f), g1));
        }
        return;
    }
    float d = (g1 - g0) / (float)frames;
    for (unsigned f = 0; f < frames; f++) {
        buf[f] *= g0 + d * (float)(f + 1);
    }
}

/* Debug builds: every sample read is checked against the playing voice's
 * plane and guard frames (chk_voice sets the window). */
#ifdef A8_DEBUG
static const int32_t *chk_lo, *chk_hi;
#define CHK_SPAN(p, n) do { if ((p) < chk_lo || (p) + (n) > chk_hi) DIAG_TRAP(TRAP_SV_READ); } while (0)
#else
#define CHK_SPAN(p, n) ((void)0)
#endif

/* Machine Bits: levels per unit amplitude. */
static float bits_levels(int bits)
{
    return fast_exp2((float)bits * 0.1f - 1.0f);
}

/* Quantises buf to q levels per unit (iq = 1 / q). */
static void apply_bits(float *buf, unsigned frames, float q, float iq)
{
    const float32x4_t iqv = vdupq_n_f32(iq);
    const uint32x4_t sign = vdupq_n_u32(0x80000000u), half = vreinterpretq_u32_f32(vdupq_n_f32(0.5f));
    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t s = vmulq_n_f32(vld1q_f32(buf + f), q);
        float32x4_t h = vreinterpretq_f32_u32(vorrq_u32(half, vandq_u32(vreinterpretq_u32_f32(s), sign)));
        vst1q_f32(buf + f, vmulq_f32(vcvtq_f32_s32(vcvtq_s32_f32(vaddq_f32(s, h))), iqv));
    }
}

/* Samples (stock, measured): mid-rise levels 2^(1 - 1.00845 bits) apart
 * (fit over 1-12 bits, within 0.2 %), no zero level, so 1 bit is a
 * full-scale square; after interpolation, before the level. No dither. */
/* d[f] = a[f] * b[f] (NEON; the same products as VFP with FZ/DN set). */
static void mul_frames(float *d, const float *a, const float *b, unsigned n)
{
    unsigned f = 0;

    for (; f + 4 <= n; f += 4) {
        vst1q_f32(d + f, vmulq_f32(vld1q_f32(a + f), vld1q_f32(b + f)));
    }
    for (; f < n; f++) {
        d[f] = a[f] * b[f];
    }
}

/* d[f] = a[f] * g */
static void scale_frames(float *d, const float *a, float g, unsigned n)
{
    unsigned f = 0;

    for (; f + 4 <= n; f += 4) {
        vst1q_f32(d + f, vmulq_n_f32(vld1q_f32(a + f), g));
    }
    for (; f < n; f++) {
        d[f] = a[f] * g;
    }
}

static void sample_bits(float *buf, unsigned n, int bits)
{
    if (bits >= BITS_MAX) {
        return;
    }
    const float32x4_t q = vdupq_n_f32(fast_exp2((float)bits * 0.100845f - 1.0f)), iq = vdupq_n_f32(1.0f / vgetq_lane_f32(q, 0));
    const float32x4_t lo = vdupq_n_f32(-1.0f), hi = vdupq_n_f32(0.99999994f), half = vdupq_n_f32(0.5f);
    for (unsigned f = 0; f < n; f += 4) {
        float32x4_t x = vmulq_f32(vminq_f32(vmaxq_f32(vld1q_f32(buf + f), lo), hi), q);
        float32x4_t t = vcvtq_f32_s32(vcvtq_s32_f32(x));
        /* floor: one less where truncation went up (x - t < 0) */
        t = vaddq_f32(t, vcvtq_f32_s32(vshrq_n_s32(vreinterpretq_s32_f32(vsubq_f32(x, t)), 31)));
        vst1q_f32(buf + f, vmulq_f32(vaddq_f32(t, half), iq));
    }
}

/* ---- sampler ---- */

#define SAMPLE_SCALE    1.0f
#define ENV_OFF         1e-5f
#define END_DECAY       0.996175f
#define FRAME           ((int64_t)1 << 32)
#define SMAX_STEP       (64 * FRAME)

void engine_samples_stop(void)
{
    uint32_t f;

    __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(f) :: "memory");
    for (int ch = 0; ch < CHANNELS; ch++) {
        sv[ch].on = 0;
    }
    __asm__ volatile("msr cpsr_c, %0" :: "r"(f) : "memory");
}

/* A voice's plane is still its sample's (not unloaded or reloaded
 * elsewhere since it was picked up). */
static int sv_current(const struct svoice *v)
{
    const struct sample *sm;

    if (!samples_valid(v->smp)) {
        return 0;
    }
    sm = &samples[v->smp];
    return sm->state == SMP_READY && v->x && (v->x == sm->plane[0] || v->x == sm->plane[1]) &&
           v->pmax == ((int64_t)sm->frames - 1) * FRAME;
}

#ifdef A8_DEBUG
/* Positions may overshoot a limit by up to a step (the voice then wraps or
 * ends without reading there). */
static void chk_voice(const struct svoice *v)
{
    const struct sample *sm = &samples[v->smp];
    int64_t top = (int64_t)sm->frames * FRAME;

    if (!v->x || (const uint8_t *)(v->x - SAMPLE_GUARD) < samples_pool_lo() ||
        (const uint8_t *)(v->x + sm->frames + SAMPLE_GUARD) > samples_pool_hi()) {
        DIAG_TRAP(TRAP_SV_PLANE);
    }
    if (v->start < 0 || v->end > top || v->start >= v->end || v->lstart < 0 || v->lend > top ||
        v->lstart > v->lend || v->pos < -(int64_t)SMAX_STEP || v->pos > top + SMAX_STEP) {
        DIAG_TRAP(TRAP_SV_POS);
    }
    chk_lo = v->x - SAMPLE_GUARD;
    chk_hi = v->x + sm->frames + SAMPLE_GUARD;
}
#define CHK_VOICE(v) chk_voice(v)
#else
#define CHK_VOICE(v) ((void)0)
#endif

static const struct sample *zone_sample(int zch, int z)
{
    int i = preset.ch[zch].s.zone[z].smp - 1;

    if (!samples_valid(i) || samples[i].state != SMP_READY) {
        return 0;
    }
    return &samples[i];
}

static int zone_count(int zch)
{
    int n = 0;

    while (n < ZONES && preset.ch[zch].s.zone[n].smp) {
        n++;
    }
    return n;
}

static int zone_for(const struct sampler_cfg *s, const struct zone_cfg *zones, int pc, int n, int zch)
{
    int lock = engine_zone_lock[zch], mv = (int)(engine_cv(s->zones_cv, pc) * 100.0f + (s->zones_cv ? 0.5f : 0.0f));

    if (lock && lock <= n) {
        return lock - 1;
    }
    for (int z = 0; z < n - 1; z++) {
        if (mv >= zones[z].minv) {
            return z;
        }
    }
    return n - 1;
}

/* Loads zone z of channel zch (its sample, side and positions; s gives
 * the channel parameters) into v; the position is kept unless start. */
/* m x unit; skips libgcc's int64 conversions when there is no mod. */
static int64_t pmod_at(float m, int64_t unit)
{
    return m == 0.0f ? 0 : (int64_t)(m * (float)unit);
}

static int sv_zone(struct svoice *v, const struct sampler_cfg *s, const struct zone_cfg *zones, int zch,
                   int z, int start)
{
    const struct sample *sm = zone_sample(zch, z);
    const struct zone_cfg *zc = &zones[z];

    if (!sm) {
        return 0;
    }
    /* Position mods (stock, measured): a fifth of the sample per volt at
     * 1.00; the start is read at the trigger, the others live. */
    int64_t fr = sm->frames, unit = fr * FRAME / 5;
    int64_t a = (int64_t)zc->start * FRAME + pmod_at(v->pmod[0], unit);
    int64_t e = (zc->end == POS_END ? fr : zc->end) * FRAME + pmod_at(v->pmod[1], unit);
    e = e > fr * FRAME ? fr * FRAME : e < FRAME ? FRAME : e;
    a = a >= e ? e - FRAME : a < 0 ? 0 : a;
    a &= ~(FRAME - 1);
    e &= ~(FRAME - 1);
    if (e <= 0) {
        return 0;
    }
    /* LoopLength is a length even with LoopLengthIsEnd (stock, measured):
     * that flag only says which end stays put when the start moves. A
     * modulated start keeps the length and stops where the end meets the
     * sample's end. */
    int64_t ls = (int64_t)zc->loop_start * FRAME + pmod_at(v->pmod[2], unit), len;
    if (zc->loop_len == POS_END) {
        len = fr * FRAME - (int64_t)zc->loop_start * FRAME;
    } else {
        len = (int64_t)zc->loop_len << 27;
    }
    int64_t le;
    if (v->pmod[2] == 0.0f && v->pmod[3] == 0.0f) {
        le = ls + len > fr * FRAME ? fr * FRAME : ls + len;
        ls = ls > le - 4 * FRAME ? le - 4 * FRAME : ls;
        ls = ls < 0 ? 0 : ls;
    } else {
        len += pmod_at(v->pmod[3], unit);
        len = len < 4 * FRAME ? 4 * FRAME : len > fr * FRAME ? fr * FRAME : len;
        ls = ls > fr * FRAME - len ? fr * FRAME - len : ls < 0 ? 0 : ls;
        le = ls + len;
        le = le > fr * FRAME ? fr * FRAME : le;
    }
    v->zone = (uint8_t)z;
    v->smp = (int16_t)(sm - samples);
    v->x = sm->plane[zc->side & 1];
    v->pmax = ((int64_t)fr - 1) * FRAME;
    v->start = a;
    v->end = e;
    v->lstart = ls;
    v->lend = le;
    v->rev = s->reverse ^ revcv[zch];
    /* Reads are half a frame before the start in the direction of play
     * (stock, measured: bit-exact against its output; every frame at
     * unison is the average of two); the trigger frame already reads one
     * step on. */
    if (start) {
        v->pos = v->rev ? (v->end - FRAME / 2 > a ? v->end - FRAME / 2 : a) : v->start - FRAME / 2;
    }
    if (v->pos < -FRAME / 2 || v->pos > (v->rev ? v->pmax + FRAME / 2 : fr * FRAME)) {
        v->pos = v->pos < 0 ? -FRAME / 2 : v->rev ? v->pmax + FRAME / 2 : fr * FRAME;
    }
    CHK_VOICE(v);
    return 1;
}

/* Splice smoothing (stock, measured): the step at a splice is added back
 * to the output and decays by 1/64 a frame (time constant 64 frames). */
#define SPLICE_K        (63.0f / 64.0f)

static float sv_value(const int32_t *x, int64_t pos, int k)
{
    return interp_at(x, pos, k);
}

static void sv_splice(struct svoice *v, float old, int64_t pos)
{
    v->sd += old - sv_value(v->x, pos, v->k);
}

static void sv_smooth(struct svoice *v, float *out, unsigned n)
{
    float d = v->sd;

    if (d == 0.0f) {
        return;
    }
    /* Ends at a frame, not at the end of a call: the same in any block size. */
    for (unsigned f = 0; f < n && d != 0.0f; f++) {
        out[f] += d;
        d *= SPLICE_K;
        d = d * d < 1e-12f ? 0.0f : d;
    }
    v->sd = d;
}

static void sv_trigger(struct svoice *v, const struct sampler_cfg *s, const struct zone_cfg *zones, int zch,
                       int pc, int n)
{
    static uint32_t rng = 22222;
    int z;

    if (engine_zone_lock[zch] && engine_zone_lock[zch] <= n) {
        z = engine_zone_lock[zch] - 1;
    } else if (s->zones_rt == ZSEL_RANDOM) {
        rng = rng * 1664525u + 1013904223u;
        z = (int)((rng >> 16) % (uint32_t)n);
    } else if (s->zones_rt == ZSEL_ADVANCE) {
        z = v->adv % n;
        v->adv = (uint8_t)((z + 1) % n);
    } else {
        z = zone_for(s, zones, pc, n, zch);
    }
    float old = s->splice && v->on && !v->tail ? sv_value(v->x, v->pos > v->pmax ? v->pmax : v->pos, v->k) : 0.0f;
    if (!sv_zone(v, s, zones, zch, z, 1)) {
        v->on = 0;
        return;
    }
    if (s->splice && v->on && !v->tail) {
        sv_splice(v, old, v->pos);
    } else {
        v->sd = 0.0f;
    }
    if (!s->attack_from_current || !v->on) {
        v->env = 0.0f;
    }
    v->on = 1;
    v->tail = 0;
    v->stage = ENV_ATTACK;
    v->looping = s->loop_mode != LOOP_OFF;
}

/* While a voice loops, reads at positions inside the loop see it as
 * periodic (stock's taps read past the loop, so a single-cycle loop clicks
 * every cycle; ours is deliberately better). A loop that fits is copied
 * once per call with its wraps around it; a longer one per run of reads
 * near its ends. */
#define WIN_MAX         (AUDIO_BLOCK * 64 + 2 * SAMPLE_GUARD + 4)

struct swin {
    const int32_t *loop;        /* the whole loop's copy, frame 0; 0 if too long */
    uint8_t tried;
    int32_t w[WIN_MAX];
};

/* Frames a..b, a read of the loop ls..le - 1 seen as periodic, into d. */
static int32_t *sv_copy(const int32_t *x, int ls, int le, int a, int b, int32_t *d)
{
    int len = le - ls;

    for (int i = a; i <= b;) {
        int src = i < ls ? le - 1 - (ls - 1 - i) % len : i < le ? i : ls + (i - le) % len;
        int n = i < ls ? ls - i : le - src;
        n = n < le - src ? n : le - src;
        n = n < b - i + 1 ? n : b - i + 1;
        const int32_t *p = x + src;
        int k = 0;
        for (; k + 4 <= n; k += 4) {
            vst1q_s32(d + k, vld1q_s32(p + k));
        }
        for (; k < n; k++) {
            d[k] = p[k];
        }
        d += n;
        i += n;
    }
    return d;
}

/* Where frame base is read from for reads (all inside the loop) touching
 * frames base + lo .. base + hi. */
static const int32_t *sv_wrapped(const struct svoice *v, struct swin *s, int64_t base, int lo, int hi)
{
    int ls = (int)(v->lstart >> 32), le = (int)((v->lend + FRAME - 1) >> 32), len = le - ls;
    int a = (int)base + lo, b = (int)base + hi;

    if ((b < le && a >= ls) || len < 4) {
        return v->x + base;
    }
    /* The first run copies only its own frames when that is less. */
    if (!s->tried && hi - lo + 1 < len + INTERP_REACH_LO + INTERP_REACH_HI) {
        s->tried = 1;
        s->loop = 0;
    } else if (s->tried < 2) {
        s->tried = 2;
        s->loop = len + INTERP_REACH_LO + INTERP_REACH_HI <= WIN_MAX ?
                  sv_copy(v->x, ls, le, ls - INTERP_REACH_LO, le - 1 + INTERP_REACH_HI, s->w), s->w + INTERP_REACH_LO - ls : 0;
    }
#ifdef A8_DEBUG
    chk_lo = s->w;
    chk_hi = s->loop ? s->w + len + INTERP_REACH_LO + INTERP_REACH_HI : s->w + (hi - lo + 1);
#endif
    if (s->loop) {
        return s->loop + base;
    }
    if (hi - lo + 1 > WIN_MAX) {
        return v->x + base;
    }
    sv_copy(v->x, ls, le, a, b, s->w);
    return s->w - lo;
}

#ifdef A8_DEBUG
#define CHK_UNWINDOW(v) chk_voice(v)
#else
#define CHK_UNWINDOW(v) ((void)0)
#endif

static int sv_in_loop(const struct svoice *v, int64_t pos)
{
    return v->looping && pos >= v->lstart && pos < v->lend;
}

static void sv_run(const struct svoice *v, const int32_t *x, const int64_t *q, const uint16_t *kk, float *out,
                   unsigned n)
{
#ifdef A8_DEBUG
    for (unsigned f = 0; f < n; f++) {
        CHK_SPAN(x + (q[f] >> 32) - INTERP_REACH_LO, INTERP_REACH_LO + INTERP_REACH_HI + 1);
    }
#endif
    interp_run(v - sv, x, q, kk, out, n);
    CHK_UNWINDOW(v);
}

/* interp_run over positions q (relative to frame base, monotonic if mono)
 * that may leave the loop: runs inside it read it periodically, runs
 * outside read the sample. */
static void sv_interp(struct svoice *v, int64_t base, const int64_t *q, const uint16_t *kk, float *out, unsigned n,
                      int mono, struct swin *win)
{
    int64_t qa = q[0] < q[n - 1] ? q[0] : q[n - 1], qb = q[0] < q[n - 1] ? q[n - 1] : q[0];

    for (unsigned f = 1; !mono && f < n; f++) {
        qa = q[f] < qa ? q[f] : qa;
        qb = q[f] > qb ? q[f] : qb;
    }
    int64_t pa = base * FRAME + qa, pb = base * FRAME + qb;
    if (!v->looping || pb < v->lstart || pa >= v->lend ||
        (base + (qa >> 32) - INTERP_REACH_LO >= v->lstart >> 32 &&
         base + (qb >> 32) + INTERP_REACH_HI < (v->lend + FRAME - 1) >> 32)) {
        sv_run(v, v->x + base, q, kk, out, n);
        return;
    }
    const int64_t ls = v->lstart - base * FRAME, le = v->lend - base * FRAME;
    for (unsigned f = 0, e; f < n; f = e) {
        int in = q[f] >= ls && q[f] < le;
        qa = qb = q[f];
        if (mono) {
            /* steps < 64 frames: a run never spans AUDIO_BLOCK * 64 */
            for (e = f + 1; e < n && (q[e] >= ls && q[e] < le) == in; e++) {
            }
            qa = q[f] < q[e - 1] ? q[f] : q[e - 1];
            qb = q[f] < q[e - 1] ? q[e - 1] : q[f];
        } else {
            for (e = f + 1; e < n && (q[e] >= ls && q[e] < le) == in; e++) {
                int64_t a = q[e] < qa ? q[e] : qa, b = q[e] > qb ? q[e] : qb;
                if ((b >> 32) - (a >> 32) > AUDIO_BLOCK * 64) {
                    break;
                }
                qa = a;
                qb = b;
            }
        }
        const int32_t *x = in ? sv_wrapped(v, win, base, (int)(qa >> 32) - INTERP_REACH_LO,
                                           (int)(qb >> 32) + INTERP_REACH_HI) : v->x + base;
        sv_run(v, x, q + f, kk + f, out + f, e - f);
    }
}

/* The next frame (read at v->pos + ds) is past the loop/end: wraps it
 * (returns 1 to go on; v->pos is then one ds short of the wrapped read) or
 * starts the tail. */
static int sv_boundary(struct svoice *v, int64_t ds)
{
    int64_t pos = v->pos + ds, w = pos;

    DIAG_IRQ(DT_IRQ_SV_BOUNDARY, v - sv);

    float old = v->splice && v->looping ? sv_value(v->x, pos < 0 ? 0 : pos > v->pmax ? v->pmax : pos, v->k) : 0.0f;
    int wrap = 0;

    if (!v->rev) {
        if (v->looping && pos >= v->lstart && v->lend > v->lstart) {
            w = v->lstart + (pos - v->lend) % (v->lend - v->lstart);
            wrap = 1;
        } else if (v->looping && pos < v->lstart) {
            w = v->lstart;
            wrap = 1;
        }
    } else if (v->looping && pos < v->lend && v->lend > v->lstart) {
        int64_t r = (v->lstart - pos) % (v->lend - v->lstart);
        w = r ? v->lend - r : v->lstart;
        wrap = 1;
    }
    if (wrap) {
        v->pos = w - ds;
        if (v->splice) {
            sv_splice(v, old, w);
        }
        return 1;
    }
    v->tail = 1;
    return 0;
}

/* Past the end: stock holds the last value and lets it die away (time
 * constant ~5.4 ms, measured) instead of stepping to zero. */
static void sv_tail(struct svoice *v, float *out, unsigned o, unsigned n)
{
    for (unsigned f = o; f < n; f++) {
        v->last *= v->tail ? END_DECAY : 0.0f;
        out[f] = v->last;
    }
    if (v->tail && v->last * v->last < ENV_OFF * ENV_OFF) {
        v->on = 0;
    }
}

static int64_t sv_limit(const struct svoice *v)
{
    return v->rev ? (v->looping ? v->lstart : v->start) : (v->looping ? v->lend : v->end);
}

/* Plays n frames of v into out (unscaled) at a constant step (Q25, > 0);
 * returns frames written before it ended (the rest zeroed). Each frame
 * advances, then reads (stock). */
static unsigned sv_play(struct svoice *v, int32_t step, int kern, float *out, unsigned n)
{
    const int64_t d = (int64_t)step << 7, ds = v->rev ? -d : d;
    unsigned o = 0;
    int guard = 0;
    struct swin win;

    win.tried = 0;
    v->k = (uint16_t)kern;

    while (o < n && v->on && !v->tail && guard++ < 64) {
        int64_t pos = v->pos, lim = sv_limit(v), room = v->rev ? pos - lim : lim - pos - 1;
        unsigned k = n - o;

        if (room < d * k) {
            k = room < d ? 0 : (unsigned)(room / d);
        }
        if (!k) {
            if (sv_boundary(v, ds)) {
                continue;
            }
            break;
        }
        int64_t last = pos + ds * k, base = (v->rev ? last : pos + ds) >> 32;
        int64_t q[AUDIO_BLOCK];
        uint16_t kk[AUDIO_BLOCK];
        int64_t r = pos - base * FRAME;
        for (unsigned f = 0; f < k; f++) {
            r += ds;
            q[f] = r;
            kk[f] = (uint16_t)kern;
        }
        if (v->unread) {
            memset(out + o, 0, k * sizeof(*out));
        } else {
            sv_interp(v, base, q, kk, out + o, k, 1, &win);
            sv_smooth(v, out + o, k);
        }
        for (unsigned f = 0; v->trace && f < k; f++) {
            v->trace[o + f] = base * FRAME + q[f];
        }
        v->pos = last;
        o += k;
        v->last = out[o - 1];
    }
    sv_tail(v, out, o, n);
    return o;
}

/* The same with a step per frame (Q25, signed: below zero plays
 * backwards, LinFM shuttle), if off a position offset per frame (Q16
 * frames; phase mod; reads outside a loop as stock does, but stays inside
 * the sample), Aliasing per frame (ax: interp_alias_x values) and splice
 * smoothing. Backwards stops at the start (forward again from there). */
static unsigned sv_play_steps(struct svoice *v, const int32_t *steps, const int32_t *off, const float *fbi,
                              const float *fbg, const int16_t *ax, float *out, unsigned n)
{
    unsigned o = 0;
    int guard = 0;
    int64_t q[AUDIO_BLOCK];
    struct swin win;
    uint16_t kk[AUDIO_BLOCK];
    int32_t smin = 0;

    win.tried = 0;
    for (unsigned f = 0; f < n; f++) {
        uint32_t a = steps[f] < 0 ? 0u - (uint32_t)steps[f] : (uint32_t)steps[f];
        smin = steps[f] < smin ? steps[f] : smin;
        kk[f] = (uint16_t)interp_kernel(ax[f], a);
    }
    if (n) {
        v->k = kk[n - 1];
    }
    while (o < n && v->on && !v->tail && guard++ < 64) {
        int64_t p = v->pos, lim = sv_limit(v), base = p >> 32;
        unsigned k = 0;

        if (!off && !fbi && smin >= 0 && !v->rev && !v->trace) {
            const int64_t b = base * FRAME;
            while (o + k < n) {
                int64_t np = p + (int64_t)steps[o + k] * 128;
                if (np >= lim) {
                    break;
                }
                p = np;
                q[k++] = p - b;
            }
        } else if (!off && !fbi && smin >= 0) {
            while (o + k < n) {
                int64_t d = (int64_t)steps[o + k] * 128, np = v->rev ? p - d : p + d;
                if (v->rev ? np < lim : np >= lim) {
                    break;
                }
                p = np;
                if (v->trace) {
                    v->trace[o + k] = p;
                }
                q[k++] = p - base * FRAME;
            }
        }
        while (o + k < n && (off || fbi || smin < 0)) {
            int32_t st = steps[o + k];
            int64_t d = (int64_t)st * 128, np = v->rev ? p - d : p + d;
            if (st < 0) {
                int64_t b = v->rev ? (v->looping && p < v->lend ? v->lend - FRAME : v->end - FRAME) :
                                     (v->looping && p >= v->lstart ? v->lstart : v->start);
                np = v->rev ? (np > b ? b : np) : (np < b ? b : np);
            } else if (v->rev ? np < lim : np >= lim) {
                break;
            }
            p = np;
            if (v->trace) {
                v->trace[o + k] = p;
            }
            int64_t a = p;
            if (off || fbi) {
                a += (int64_t)(off ? off[o + k] : (int32_t)(fbi[o + k] * v->fb * 65536.0f)) * 65536;
                a = a < 0 ? 0 : a > v->pmax ? v->pmax : a;
            }
            q[k] = a - base * FRAME;
            if (fbi) {
                int i = (int)(q[k] >> 32);
                const int32_t *x = sv_in_loop(v, a) ?
                                   sv_wrapped(v, &win, base, i - INTERP_REACH_LO, i + INTERP_REACH_HI) : v->x + base;
                CHK_SPAN(x + i - INTERP_REACH_LO, INTERP_REACH_LO + INTERP_REACH_HI + 1);
                out[o + k] = interp_at(x, q[k], kk[o + k]) + v->sd;
                CHK_UNWINDOW(v);
                v->sd *= SPLICE_K;
                v->fb = out[o + k] * fbg[o + k];
            }
            k++;
            if (p - base * FRAME > ((int64_t)1 << 46) || p - base * FRAME < -((int64_t)1 << 46)) {
                break;
            }
        }
        if (!k) {
            int64_t d = (int64_t)steps[o] * 128;
            if (sv_boundary(v, v->rev ? -d : d)) {
                continue;
            }
            break;
        }
        if (!fbi && v->unread) {
            memset(out + o, 0, k * sizeof(*out));
        } else if (!fbi) {
            sv_interp(v, base, q, kk + o, out + o, k, !off && smin >= 0, &win);
            sv_smooth(v, out + o, k);
        }
        v->pos = p;
        o += k;
        v->last = out[o - 1];
    }
    sv_tail(v, out, o, n);
    return o;
}

/* Stock's envelope (measured in the emulator): the attack approaches full
 * level exponentially with a time constant of Attack; the release falls
 * 47.5 dB per Release time, exponentially. Per-frame factors. */
#define REL_DB          47.2f

static float env_attack(uint32_t tenth_ms)
{
    return tenth_ms ? 1.0f - fast_exp2(-14426.95f / ((float)tenth_ms * AUDIO_RATE)) : 1.0f;
}

static float env_release(uint32_t tenth_ms)
{
    return tenth_ms ? fast_exp2(-REL_DB * 0.166096f * 10000.0f / ((float)tenth_ms * AUDIO_RATE)) : 0.0f;
}

/* Frames of a block (bit per frame) trigger source src was held. */
static uint32_t held_word(int src, uint32_t block)
{
    const struct held_hist *h = &held_hist[block % HELD_BLOCKS];

    if (src < 8) {
        return h->gate[src];
    }
    return (src < TRIG_SRC_LATCH ? (h->buttons >> (src - 8)) & 1 :
            src < TRIG_SRC_AUTO ? (h->latched >> (src - TRIG_SRC_LATCH)) & 1 : 0) ? ~0u : 0u;
}

/* Frames of this block the gate, button or latch that fired out_ch is
 * held, as it was the note's start delay ago. */
static uint32_t fired_held(int out_ch)
{
    int src = engine_status.trigger_src[out_ch];
    unsigned d = note_delay[out_ch], q = d / AUDIO_BLOCK, r = d % AUDIO_BLOCK;
    uint32_t a = held_word(src, block_now - q);

    return r ? a << r | held_word(src, block_now - q - 1) >> (AUDIO_BLOCK - r) : a;
}

/* Frames from `from` (of n) that held stays set. */
static unsigned held_run(uint32_t held, unsigned from, unsigned n)
{
    uint32_t m = ~(held >> from);
    unsigned k = m ? (unsigned)__builtin_ctz(m) : 32u;

    return k < n ? k : n;
}

/* Per frame: attack e += (1 - e) a; release e *= r while above ENV_OFF,
 * then 0 (ended); buf *= e. In NEON lanes: with FPSCR's flush-to-zero and
 * default NaN the same results as VFP, without its unpipelined latency. */
static float env_run(struct svoice *v, float e, float a, float r, float *buf, unsigned from, unsigned n, int *ended)
{
    float32x2_t ev = vdup_n_f32(e);

    if (v->stage == ENV_ATTACK) {
        const float32x2_t one = vdup_n_f32(1.0f), av = vdup_n_f32(a);
        if (vget_lane_f32(vmla_f32(ev, vsub_f32(one, ev), av), 0) == e) {
            /* settled (it stops one step short of 1.0): a constant gain */
            unsigned f = from;
            for (; f + 4 <= n; f += 4) {
                vst1q_f32(buf + f, vmulq_n_f32(vld1q_f32(buf + f), e));
            }
            for (; f < n; f++) {
                buf[f] *= e;
            }
            return e;
        }
        for (unsigned f = from; f < n; f++) {
            ev = vmla_f32(ev, vsub_f32(one, ev), av);
            vst1_lane_f32(buf + f, vmul_f32(vld1_dup_f32(buf + f), ev), 0);
        }
    } else {
        const float32x2_t rv = vdup_n_f32(r), off = vdup_n_f32(ENV_OFF);
        uint32x2_t live = vdup_n_u32(~0u);
        for (unsigned f = from; f < n; f++) {
            /* e >= 0: ordered as its bits (vcgt_f32 compiles to VFP compares) */
            uint32x2_t m = vcgt_s32(vreinterpret_s32_f32(ev), vreinterpret_s32_f32(off));
            live = vand_u32(live, m);
            ev = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(vmul_f32(ev, rv)), m));
            vst1_lane_f32(buf + f, vmul_f32(vld1_dup_f32(buf + f), ev), 0);
        }
        if (!vget_lane_u32(live, 0)) {
            *ended = 1;
        }
    }
    return vget_lane_f32(ev, 0);
}

/* Envelope over buf[from..n) and the gate logic; held: frames still held.
 * Returns 1 if the voice dies away within, to stop after it has played. */
static int sv_env(struct svoice *v, const struct sampler_cfg *s, unsigned held, float *buf, unsigned from, unsigned n,
                  float at_k, float rl_k)
{
    uint32_t at = (uint32_t)((float)s->attack * at_k), rl = (uint32_t)((float)s->release * rl_k);
    if (at != v->at_ms || !v->at_c) {
        v->at_ms = at;
        v->at_c = env_attack(at);
    }
    if (rl != v->rl_ms || !v->rl_c) {
        v->rl_ms = rl;
        v->rl_c = env_release(rl);
    }
    float a = v->at_c, r = v->rl_c;
    float e = v->env;
    int src = engine_status.trigger_src[v - sv], ended = 0;
    unsigned k = held < from ? from : held < n ? held : n;

    e = env_run(v, e, a, r, buf, from, k, &ended);
    /* Gated: the gate's fall releases. One shot: only an unlatch does. */
    if (k < n && v->stage != ENV_RELEASE &&
        (s->play_mode == PLAY_GATED || (src >= TRIG_SRC_LATCH && src < TRIG_SRC_AUTO))) {
        v->stage = ENV_RELEASE;
    }
    /* Loop gated, either play mode: the gate's fall leaves the loop at its
     * end and the rest of the sample plays (stock). */
    if (k < n && s->loop_mode == LOOP_GATED) {
        v->looping = 0;
    }
    v->env = env_run(v, e, a, r, buf, k, n, &ended);
    return ended;
}

/* Stock's scales at 1.00, measured in the emulator: PitchCV 2 oct/V,
 * ExpFM 0.2 oct/V; LinFM adds 0.4 frames per output frame per volt whatever
 * the pitch and the file's rate (below zero it plays backwards: shuttle);
 * ExpAM 6 dB per 5 V;
 * LinAM the level times 1 + 0.2/V. */
static float smod(const struct sampler_cfg *s, int i, int pc)
{
    return s->mod[i].src ? engine_cv(s->mod[i].src, pc) * (float)s->mod[i].amt * 0.01f : 0.0f;
}

/* A route's voltage at each frame of the block, 0 when off. */
static const float *route_frames(int src, int ch)
{
    if (src >= CV_REL_A && src <= CV_REL_C) {
        return cv_frames(ch * 3 + (src - CV_REL_A));
    }
    if (src >= CV_ABS && src < CV_SRC_COUNT) {
        return cv_frames(src - CV_ABS);
    }
    return 0;
}

/* Audio-rate routes of a sample voice: pitch (PitchCV, ExpFM, LinFM) and
 * level (MLevelMod, ExpAM, LinAM) per frame. */
struct sroutes {
    const float *pitch, *expfm, *linfm, *level, *expam, *linam;
    int32_t ap, ap3, ae;        /* PitchCV, ExpFM amounts (Q15) */
    int16_t al;                 /* LinFM */
    float kv, kx, ka;
};

static void sroutes_get(struct sroutes *r, const struct channel_cfg *c, int pc)
{
    const struct sampler_cfg *s = &c->s;

    r->pitch = route_frames(c->mod[D_PITCH].src, pc);
    r->expfm = route_frames(s->mod[SM_EXPFM].src, pc);
    r->linfm = route_frames(s->mod[SM_LINFM].src, pc);
    r->level = route_frames(c->mod[D_LEVEL].src, pc);
    r->expam = route_frames(s->mod[SM_EXPAM].src, pc);
    r->linam = route_frames(s->mod[SM_LINAM].src, pc);
    r->ap = pitch_amount(c->mod[D_PITCH].amt);
    r->ap3 = (r->ap + 4) >> 3;
    r->ae = pitch_amount(s->mod[SM_EXPFM].amt);
    r->al = (int16_t)pitch_amount(s->mod[SM_LINFM].amt);
    r->kv = per_volt(D_LEVEL) * (float)c->mod[D_LEVEL].amt * 0.01f;
    r->kx = 12.04f * (float)s->mod[SM_EXPAM].amt * 0.01f;
    r->ka = 0.2f * (float)s->mod[SM_LINAM].amt * 0.01f;
}

/* fast_exp2 on 4 lanes (same polynomial), -32 < x < 32. Float vector
 * compares would be scalarised by GCC (not IEEE on NEON), so floor() is a
 * truncation of x + 32. */
static inline float32x4_t exp2_4(float32x4_t x)
{
    int32x4_t i = vsubq_s32(vcvtq_s32_f32(vaddq_f32(x, vdupq_n_f32(32.0f))), vdupq_n_s32(32));
    float32x4_t xi = vcvtq_f32_s32(i);
    float32x4_t f = vsubq_f32(x, xi);
    float32x4_t p = vmlaq_n_f32(vdupq_n_f32(0.0013333f), f, 0.0001540f);
    p = vmlaq_f32(vdupq_n_f32(0.0096181f), p, f);
    p = vmlaq_f32(vdupq_n_f32(0.0555041f), p, f);
    p = vmlaq_f32(vdupq_n_f32(0.2402265f), p, f);
    p = vmlaq_f32(vdupq_n_f32(0.6931472f), p, f);
    p = vmlaq_f32(vdupq_n_f32(1.0f), p, f);
    int32x4_t e = vshlq_n_s32(i, 23);
    return vreinterpretq_f32_s32(vaddq_s32(vreinterpretq_s32_f32(p), e));
}

/* CV volts as stock's signed 16-bit values (1 V = 6553.6), in 32-bit lanes. */
static inline int32x4_t cv16(const float *v)
{
    float32x4_t x = vmulq_n_f32(vld1q_f32(v), 6553.6f);
    float32x4_t h = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u)),
                                                    vreinterpretq_u32_f32(vdupq_n_f32(0.5f))));
    return vmovl_s16(vqmovn_s32(vcvtq_s32_f32(vaddq_f32(x, h))));
}

/* Per-frame steps (Q25) over the whole block from the log-pitch lp
 * (pitch.h): PitchCV and ExpFM added to it, LinFM to the step. */
static void sroutes_steps(const struct sroutes *r, int32_t lp, int32_t *steps, unsigned frames)
{
    const int32x4_t base = vdupq_n_s32(lp);

    for (unsigned f = 0; f < frames; f += 4) {
        int32x4_t e = vdupq_n_s32(0), cp = e;
        if (r->expfm) {
            e = vrshrq_n_s32(vmulq_n_s32(cv16(r->expfm + f), r->ae), 3);
        }
        if (r->pitch) {
            cp = cv16(r->pitch + f);
            e = vaddq_s32(e, vmulq_n_s32(cp, r->ap));
        }
        e = vmlaq_n_s32(vrshrq_n_s32(e, 1), cp, r->ap3);
        int32x4_t st = pitch_step4(vqaddq_s32(e, base));
        if (r->linfm) {
            int16x4_t l = vqrdmulh_n_s16(vmovn_s32(cv16(r->linfm + f)), r->al);
            st = vqaddq_s32(st, vshll_n_s16(l, 11));
        }
        vst1q_s32(steps + f, st);
    }
}

/* buf[from..to) = pre[0..) times per-frame gains over the whole block. */
static void sroutes_gain(const struct sroutes *r, const struct channel_cfg *c, int zone_tenths, float *buf,
                         const float *pre, unsigned from, unsigned to)
{
    const float32x4_t lo = vdupq_n_f32(SLEVEL_MIN), hi = vdupq_n_f32(LEVEL_MAX), zero = vdupq_n_f32(0.0f);
    const float32x4_t base = vdupq_n_f32((float)c->level), zl = vdupq_n_f32((float)zone_tenths);
    const float32x4_t kv = vdupq_n_f32(r->kv), kx = vdupq_n_f32(r->kx), ka = vdupq_n_f32(r->ka);
    const float32x4_t kdb = vdupq_n_f32(0.1f / 6.0205999f), sc = vdupq_n_f32(SAMPLE_SCALE);
    const float32x4_t am0 = vdupq_n_f32(c->s.linam_ext_env ? 0.0f : 1.0f);
    const int32x4_t off_db = vdupq_n_s32(SLEVEL_MIN);
    float g[AUDIO_BLOCK] __attribute__((aligned(16)));
    unsigned frames = (to + 3u) & ~3u;

    for (unsigned f = from & ~3u; f < frames; f += 4) {
        float32x4_t db = base;
        if (r->level) {
            db = vmlaq_f32(db, vld1q_f32(r->level + f), kv);
        }
        db = vaddq_f32(vminq_f32(vmaxq_f32(db, lo), hi), zl);
        if (r->expam) {
            db = vmlaq_f32(db, vld1q_f32(r->expam + f), kx);
        }
        uint32x4_t off = vcleq_s32(vcvtq_s32_f32(db), off_db);
        float32x4_t gv = vmulq_f32(exp2_4(vmulq_f32(db, kdb)), sc);
        if (r->linam) {
            gv = vmulq_f32(gv, vmaxq_f32(vmlaq_f32(am0, vld1q_f32(r->linam + f), ka), zero));
        } else {
            gv = vmulq_f32(gv, am0);
        }
        off = vorrq_u32(off, vmvnq_u32(vceqq_f32(gv, gv)));       /* NaN: silent */
        vst1q_f32(g + f, vreinterpretq_f32_u32(vbicq_u32(vreinterpretq_u32_f32(gv), off)));
    }
    if (!from) {
        for (unsigned f = 0; f < frames; f += 4) {
            vst1q_f32(buf + f, vmulq_f32(vld1q_f32(pre + f), vld1q_f32(g + f)));
        }
        return;
    }
    for (unsigned f = from; f < to; f++) {
        buf[f] = pre[f - from] * g[f];
    }
}

/* Phase mod: PMIndex 1.00 = 256 frames, PMIndexMod 51.2 frames/V at 1.00
 * (stock); the modulator at full scale (a channel before its level, a
 * sampling input; a CV at PM_CV_FS volts) moves the read position by the
 * index. */
#define PM_FRAMES       256.0f
#define PM_MOD_FRAMES   51.2f
#define PM_CV_FS        5.0f

static const float *pm_channel(int src)
{
    return chmod[(chout_done >> src) & 1 ? chout_cur : chout_cur ^ 1u][src];
}

/* Offsets for each frame of the block: 1 (offq, Q16 frames);
 * 0 when phase mod is off; 2 for the channel itself as the source (off =
 * the index as floats, frames per unit of its own last output frame). */
static int pm_offsets(const struct channel_cfg *c, int out_ch, int pc, float *off, int32_t *offq, unsigned frames)
{
    float in[AUDIO_BLOCK] __attribute__((aligned(16)));
    const struct sampler_cfg *s = &c->s;
    const float *iv = route_frames(s->mod[SM_PMINDEX].src, pc), *m = 0;
    float base = (float)s->pm_index * (PM_FRAMES / 100.0f), ik = PM_MOD_FRAMES * (float)s->mod[SM_PMINDEX].amt * 0.01f;
    float mk = 1.0f;
    int src = s->pm_source;

    if (!s->pm_index && !iv) {
        return 0;
    }
    if (src == out_ch) {
        m = 0;
    } else if (src < CHANNELS) {
        m = pm_channel(src);
    } else if (src == PM_CV) {
        m = route_frames(s->mod[SM_PHASE].src, pc);
        mk = (float)s->mod[SM_PHASE].amt * (0.01f / PM_CV_FS);
        if (!m) {
            return 0;
        }
    } else if (src == PM_LEFT || src == PM_RIGHT) {
        int side = src == PM_RIGHT;
        for (unsigned f = 0; f < frames; f++) {
            in[f] = (float)sample_in[f][side] * (1.0f / 2147483648.0f);
        }
        m = in;
    } else {
        return 0;
    }
    const float32x4_t zero = vdupq_n_f32(0.0f), top = vdupq_n_f32(PM_FRAMES);
    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t ix = vdupq_n_f32(base);
        if (iv) {
            ix = vminq_f32(vmaxq_f32(vmlaq_n_f32(ix, vld1q_f32(iv + f), ik), zero), top);
        }
        if (m) {
            vst1q_s32(offq + f, vcvtq_n_s32_f32(vmulq_f32(vmulq_n_f32(vld1q_f32(m + f), mk), ix), 16));
        } else {
            vst1q_f32(off + f, ix);
        }
    }
    return m ? 1 : 2;
}

/* Grains (Tallent, fw/tallent/grains.c) on a sample channel: its voice
 * plays on and steers them, and they replace its output before Bits and
 * the envelope. A pair's master renders both sides and the Stereo/Right
 * channel takes its own (render_order puts the master first). */
static int grain_pair(int ch)
{
    return ch + 1 < CHANNELS && preset.ch[ch + 1].mode == MODE_STEREO;
}

static int grain_dep(int ch)
{
    return preset.ch[ch].mode == MODE_STEREO && preset_param_channel(ch) == ch - 1 &&
           (fx_grain(ch - 1) || tlock_pair(ch - 1)) ? ch - 1 : -1;
}

static void grain_rest(int ch)
{
    struct grain_state *s = &grain_st[ch];

    for (unsigned n = 0; n < GRAINS; n++) {
        s->grains[n].phase_step = 0;
    }
    s->heard = GRAIN_PARKED;
}

static void grain_part(int ch, const struct grain_tuning *t, const struct svoice *v, const int64_t *trace,
                       const int32_t *steps, int32_t step, float *pre, unsigned from, unsigned n)
{
    const struct sample *sm = &samples[v->smp];
    int64_t s = v->start >> 32, e = v->end >> 32, ls = v->lstart >> 32, le = v->lend >> 32;
    int64_t a = v->rev ? e - le : ls - s, len = le - ls;
    int pair = grain_pair(ch);
    const int32_t *left = pair ? sm->plane[0] : v->x;
    struct grain_zone z;
    uint32_t at[AUDIO_BLOCK];
    int32_t st[AUDIO_BLOCK], gl[AUDIO_BLOCK], gr[AUDIO_BLOCK];

    z.pcm = v->rev ? left + e - 1 : left + s;
    z.dir = v->rev ? -1 : 1;
    z.right = pair ? (int32_t)(sm->plane[1] - sm->plane[0]) : 0;
    z.frames = (uint32_t)(e - s);
    z.loop = 0;
    z.turn = z.frames;
    if (a >= 0 && a < e - s && len > 0 && len < e - s - a) {
        z.loop = (uint32_t)a;
        z.turn = (uint32_t)(a + len);
    }
    for (unsigned f = 0; f < n; f++) {
        int64_t fr = (trace[f] + FRAME / 2) >> 32;
        at[f] = trace[f] == INT64_MIN ? GRAIN_PARKED : (uint32_t)(v->rev ? e - 1 - fr : fr - s);
        st[f] = steps ? steps[f] : step;
    }
    grain_render(&grain_st[ch], t, &z, pair, at, st, gl, gr, n);
    for (unsigned f = 0; f < n; f++) {
        pre[f] = (float)gl[f] * (1.0f / 2147483648.0f);
    }
    if (pair) {
        for (unsigned f = 0; f < n; f++) {
            grain_right[ch + 1][from + f] = (float)gr[f] * (1.0f / 2147483648.0f);
        }
        grain_right_ok |= (uint8_t)(2u << ch);
    }
}

/* How a part of a block renders the voice: at a constant step (sv_play)
 * or a step per frame; the arrays start at the part's first frame. */
struct svr {
    int per_frame, kern;
    int32_t step;
    const int32_t *steps, *po;
    const float *fbi, *ev;
    const int16_t *ax;
};

static int32_t svr_step(const struct svr *r, unsigned f)
{
    return r->per_frame ? r->steps[f] : r->step;
}

/* Frames o..o + k of the part into out[o..]. */
static void sv_render(struct svoice *v, const struct svr *r, float *out, unsigned o, unsigned k)
{
    if (r->per_frame) {
        sv_play_steps(v, r->steps + o, r->po ? r->po + o : 0, r->fbi ? r->fbi + o : 0, r->ev + o, r->ax + o, out + o, k);
    } else {
        sv_play(v, r->step, r->kern, out + o, k);
    }
}

static void tl_q31s(const float *in, int32_t *out, unsigned n)
{
    unsigned f = 0;

    for (; f + 4 <= n; f += 4) {
        vst1q_s32(out + f, vcvtq_n_s32_f32(vld1q_f32(in + f), 31));
    }
    for (; f < n; f++) {
        float x = in[f] * 2147483648.0f;
        out[f] = x >= 2147483520.0f ? 0x7fffffff : x <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)x;
    }
}

static int32_t tl_q31(float x)
{
    x *= 2147483648.0f;
    return x >= 2147483520.0f ? 0x7fffffff : x <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)x;
}

/* What rendering changes in a voice, to render a run again. */
struct sv_mark {
    int64_t pos;
    float last, fb, sd;
    uint16_t k;
    uint8_t on, tail;
};

static void sv_mark(const struct svoice *v, struct sv_mark *m)
{
    *m = (struct sv_mark){ v->pos, v->last, v->fb, v->sd, v->k, v->on, v->tail };
}

static void sv_unmark(struct svoice *v, const struct sv_mark *m)
{
    v->pos = m->pos;
    v->last = m->last;
    v->fb = m->fb;
    v->sd = m->sd;
    v->k = m->k;
    v->on = m->on;
    v->tail = m->tail;
}

/* Time Lock (Tallent, fw/src/tlock.c): the voice renders in runs up to
 * the frames where its lock may move it; each frame then goes through the
 * lock (Q31) and a move sets where the next frame reads. A run that went
 * past a move (the lock re-engaged inside it) is rendered again up to the
 * move from the voice as it was. A wait (the slice ran out) holds the voice
 * without reading it. */
static void tl_render(int ch, struct svoice *v, const struct svr *r, float *pre, unsigned from, unsigned n)
{
    struct tlock_ch L;
    int64_t trace[AUDIO_BLOCK];
    float spare[AUDIO_BLOCK];
    unsigned o = 0;

    if (!v->on || v->tail || v->rev ||
        !tlock_begin(ch, v->zone, v->start, v->end, v->lstart, v->lend, v->looping, svr_step(r, 0), &L)) {
        tlock_idle(ch);
        sv_render(v, r, pre, 0, n);
        return;
    }
    if (L.pair) {
        if (!((tl_right_ok >> (ch + 1)) & 1)) {
            for (unsigned f = 0; f < AUDIO_BLOCK; f++) {
                tl_rgain[ch + 1][f] = TL_NONE;
                tl_rwrite[ch + 1][f] = INT64_MIN;
            }
        }
        tl_right_ok |= (uint8_t)(2u << ch);
    }
    for (unsigned f = 0; f < n; f++) {
        trace[f] = INT64_MIN;
    }
    while (o < n) {
        int standing, moved = 0;
        unsigned k = timelock_quiet(L.l, L.z, L.k ? L.k + from + o : 0, n - o, &standing), i;
        struct sv_mark was;

        /* A voice that moved by itself (a trigger, a zone switch) engages
         * the lock again on its first frame: that frame alone first. */
        if (!timelock_follows(L.l, L.z, L.zone, v->pos + (int64_t)svr_step(r, o) * 128, svr_step(r, o))) {
            k = 1;
            standing = 0;
        }
        sv_mark(v, &was);

        if (standing) {
            for (i = 0; i < k; i++) {
                trace[o + i] = v->pos + (int64_t)svr_step(r, o + i) * 128;
                pre[o + i] = 0.0f;
            }
        } else {
            v->trace = trace + o;
            sv_render(v, r, pre, o, k);
            v->trace = 0;
        }
        int32_t x[AUDIO_BLOCK] __attribute__((aligned(16)));
        struct lock_out lo[AUDIO_BLOCK];
        int64_t to;
        int ended;

        unsigned stage = prof_to(PS_TL);
        tl_q31s(pre + o, x, k);
        i = timelock_run(L.l, L.z, L.zone, L.pair, L.own, L.k ? L.k + from + o : 0, trace + o,
                         r->per_frame ? r->steps + o : 0, r->step, x, lo, L.streams, k, &to, &moved);
        ended = moved < 0;
        moved = moved > 0;
        for (unsigned j = 0; j < i; j++) {
            if (lo[j].mixed) {
                pre[o + j] = (float)x[j] * (1.0f / 2147483648.0f);
            }
        }
        prof_to(stage);
        if (L.pair) {
            for (unsigned j = 0; j < i; j++) {
                tl_rgain[ch + 1][from + o + j] = lo[j].mixed ? lo[j].gain : TL_NONE;
                tl_rsum[ch + 1][from + o + j] = lo[j].right;
            }
            if (moved) {
                tl_rwrite[ch + 1][from + o + i - 1] = to;
            }
        }
        if (moved) {
            if (!standing && i < k) {
                sv_unmark(v, &was);
                sv_render(v, r, spare, o, i);
            }
            v->pos = to;
        }
        if (ended) {
            /* the voice ended: the rest plays out unlocked */
            tlock_idle(ch);
            if (o + k < n) {
                sv_render(v, r, pre, o + k, n - o - k);
            }
            return;
        }
        o += i;
    }
}

/* A Stereo/Right channel whose master's lock moved and mixed it. */
static void tl_right(int ch, struct svoice *v, const struct svr *r, float *pre, unsigned from, unsigned n)
{
    unsigned o = 0;

    while (o < n) {
        unsigned k = 1;
        while (o + k < n && tl_rwrite[ch][from + o + k - 1] == INT64_MIN) {
            k++;
        }
        sv_render(v, r, pre, o, k);
        if (tl_rwrite[ch][from + o + k - 1] != INT64_MIN && v->on && !v->tail) {
            v->pos = tl_rwrite[ch][from + o + k - 1];
        }
        o += k;
    }
    for (unsigned f = 0; f < n; f++) {
        if (tl_rgain[ch][from + f] != TL_NONE) {
            struct lock_out lo = { tl_rgain[ch][from + f], tl_rsum[ch][from + f], 1 };
            pre[f] = (float)timelock_right(tl_q31(pre[f]), &lo) * (1.0f / 2147483648.0f);
        }
    }
}

static int sampler_render(int out_ch, int pc, const struct channel_cfg *c, int trig, unsigned off,
                          float *buf, unsigned frames)
{
    /* Stereo/Right: the master's channel parameters, its own zones (stock). */
    const struct sampler_cfg *s = &c->s;
    const struct zone_cfg *zones = preset.ch[out_ch].s.zone;
    struct svoice *v = &sv[out_ch];
    int n = zone_count(out_ch);
    const struct grain_tuning *gt = out_ch == pc ? fx_grain(pc) : 0;
    int gright = out_ch == pc + 1 && fx_grain(pc);

    if (!gt) {
        grain_live &= (uint8_t)~(1u << out_ch);
    } else if (!((grain_live >> out_ch) & 1)) {
        grain_start(&grain_st[out_ch], gt, (u32)out_ch);
        grain_live |= (uint8_t)(1u << out_ch);
    }

    /* Reverse CV (ours): XOR with Reverse, Schmitt at 1.7/1.5 V, per block. */
    if (s->rev_cv) {
        float rv = engine_cv(s->rev_cv, pc);
        revcv[out_ch] = rv >= PCV_ON ? 1 : rv <= PCV_OFF ? 0 : revcv[out_ch];
    } else {
        revcv[out_ch] = 0;
    }

    if (!n) {
        v->on = 0;
        trig = 0;
    }
    if (!trig && !v->on) {
        if (gt) {
            grain_rest(out_ch);
        }
        tlock_idle(out_ch);
        return 0;
    }
    if (v->on && !sv_current(v)) {
        v->on = 0;
    }
    DIAG_IRQ(DT_IRQ_SV_ZONE, out_ch);
    if (v->on) {
        CHK_VOICE(v);
    }
    /* Realtime zone switches are always smoothed (stock); reversing while
     * playing keeps the position, so there is no step to smooth. */
    if (v->on && s->zones_rt == ZSEL_CONT) {
        int z = zone_for(s, zones, pc, n, out_ch);
        if (z != v->zone) {
            float old = sv_value(v->x, v->pos > v->pmax ? v->pmax : v->pos, v->k);
            if (!sv_zone(v, s, zones, out_ch, z, 0)) {
                v->on = 0;
            } else {
                v->pos = v->pos < 0 ? 0 : v->pos > v->pmax ? v->pmax : v->pos;
                sv_splice(v, old, v->pos);
            }
        }
    }
    /* Loop mode is live (stock): OFF leaves the loop at its end, NORMAL
     * joins it if the voice hasn't passed it; GATED follows the gate
     * (sv_env). */
    if (v->on && !v->tail) {
        if (s->loop_mode == LOOP_OFF) {
            v->looping = 0;
        } else if (s->loop_mode == LOOP_NORMAL && !v->looping && v->lend > v->lstart &&
                   (v->rev ? v->pos > v->lstart : v->pos < v->lend)) {
            v->looping = 1;
        }
    }
    /* Reverse CV (ours): flips where the voice is; a flip that lands
     * outside the loop wraps with a splice (always smoothed). */
    if (v->on && !v->tail && v->rev != (s->reverse ^ revcv[out_ch])) {
        v->rev = s->reverse ^ revcv[out_ch];
        v->pos = v->pos < 0 ? 0 : v->pos > v->pmax ? v->pmax : v->pos;
        if (v->looping && (v->rev ? v->pos < sv_limit(v) : v->pos >= sv_limit(v))) {
            uint8_t keep = v->splice;
            v->splice = 1;
            sv_boundary(v, 0);
            v->splice = keep;
        }
    }
    struct sctl *k = &sctl[out_ch];
    if (trig || !k->ok || ctl_block(out_ch)) {
        (void)modulated(c, pc, D_PITCH, c->pitch, SPITCH_MIN, 14400);
        k->level = modulated(c, pc, D_LEVEL, c->level, SLEVEL_MIN, LEVEL_MAX) + (int)(12.04f * smod(s, SM_EXPAM, pc));
        /* External envelope: 0 V silent, +5 V unity (scale assumed, unmeasured). */
        k->am = (s->linam_ext_env ? 0.0f : 1.0f) + 0.2f * smod(s, SM_LINAM, pc);
        k->bits = modulated(c, pc, D_BITS, c->bits, BITS_MIN, BITS_MAX);
        /* AttackMod/ReleaseMod: the time doubles per volt at 1.00 (stock, measured). */
        k->at_k = s->mod[SM_ATTACK].src ? fast_exp2(smod(s, SM_ATTACK, pc)) : 1.0f;
        k->rl_k = s->mod[SM_RELEASE].src ? fast_exp2(smod(s, SM_RELEASE, pc)) : 1.0f;
        for (int i = 0; i < 4; i++) {
            float m = smod(s, SM_SSTART + i, pc);
            v->pmod[i] = m >= -10.0f && m <= 10.0f ? m : 0.0f;
        }
        k->ok = 1;
        rt[out_ch].ctl_ok = 0;
    }
    int level = k->level, bits = k->bits;
    float am = k->am, at_k = k->at_k, rl_k = k->rl_k;
    unsigned at = trig ? off : frames;
    unsigned done = 0;
    float pre[AUDIO_BLOCK + 4] __attribute__((aligned(16)));
    int32_t steps[AUDIO_BLOCK] __attribute__((aligned(16)));
    uint32_t held = fired_held(out_ch);
    struct sroutes rr;

    sroutes_get(&rr, c, pc);
    int live_pts = s->mod[SM_SEND].src || s->mod[SM_LSTART].src || s->mod[SM_LLEN].src;
    if (live_pts && v->on && !trig && !sv_zone(v, s, zones, out_ch, v->zone, 0)) {
        v->on = 0;
    }
    float pmo[AUDIO_BLOCK] __attribute__((aligned(16)));
    int32_t pmq[AUDIO_BLOCK] __attribute__((aligned(16)));
    int pm = pm_offsets(c, out_ch, pc, pmo, pmq, frames);
    /* AliasingMod: 20 points per volt at 1.00, per frame (stock, measured). */
    int16_t ax[AUDIO_BLOCK];
    const float *avf = route_frames(s->mod[SM_ALIAS].src, pc);
    int alias_x = interp_alias_x(s->aliasing), alias_rt = avf && s->mod[SM_ALIAS].amt;
    float ak = 13107.2f * (float)s->mod[SM_ALIAS].amt * 0.01f;
    for (unsigned f = 0; f < frames; f++) {
        ax[f] = (int16_t)alias_x;
    }
    for (unsigned f = 0; alias_rt && f < frames; f++) {
        float x = (float)alias_x + ak * avf[f];
        ax[f] = (int16_t)(x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int)(x + (x < 0 ? -0.5f : 0.5f)));
    }
    int pitch_rt = rr.pitch || rr.expfm || rr.linfm, level_rt = rr.level || rr.expam || rr.linam;
    for (int part = 0; part < 2; part++) {
        unsigned from = part ? at : 0, to = part ? frames : at;
        if (part && trig) {
            sv_trigger(v, s, zones, out_ch, pc, n);
        }
        if (from >= to) {
            continue;
        }
        if (!v->on) {
            for (unsigned f = from; f < to; f++) {
                buf[f] = 0.0f;
            }
            continue;
        }
        const struct sample *sm = &samples[v->smp];
        const struct zone_cfg *zc = &zones[v->zone];
        if (v->rate != sm->rate) {
            v->rate = sm->rate;
            v->rate_lp = pitch_rate(sm->rate);
        }
        int32_t lp = pitch_cents(c->pitch + zc->pitch) + v->rate_lp;
        v->splice = s->splice;
        const int32_t *po = pm == 1 && !gt && !gright ? pmq + from : 0;
        const float *fbi = pm == 2 && !gt && !gright ? pmo + from : 0;
        /* The envelope first (ev), then gain x envelope into buf, so a
         * channel modulating itself can use each frame's output (before
         * the level) for the next. */
        float ev[AUDIO_BLOCK + 4] __attribute__((aligned(16)));
        for (unsigned f = 0; f < AUDIO_BLOCK + 4; f++) {
            ev[f] = 1.0f;
        }
        DIAG_IRQ(DT_IRQ_SV_ENV, out_ch);
        unsigned hold = held_run(held, from, to - from);
        int ended = sv_env(v, s, part && trig && !hold ? to - from : hold, ev, 0, to - from, at_k, rl_k);
        if (level_rt) {
            sroutes_gain(&rr, c, zc->level, buf, ev, from, to);
        } else {
            int db = level + zc->level;
            if (!k->g_ok || k->g_db != db || k->g_am != am) {
                k->g = db <= SLEVEL_MIN ? 0.0f : fast_exp2((float)db * (0.1f / 6.0205999f)) * SAMPLE_SCALE *
                                                 (am > 0.0f ? am : 0.0f);
                k->g_db = db;
                k->g_am = am;
                k->g_ok = 1;
            }
            scale_frames(buf + from, ev, k->g, to - from);
        }
        int64_t trace[AUDIO_BLOCK];
        const int32_t *gsteps = steps + from;
        int32_t gstep = 0;
        if (gt) {
            for (unsigned f = 0; f < to - from; f++) {
                trace[f] = INT64_MIN;
            }
            v->trace = trace;
        }
        v->unread = gt || gright;
        struct svr sr = { 1, 0, 0, steps + from, po, fbi, ev, ax + from };
        if (pitch_rt) {
            sroutes_steps(&rr, lp, steps, frames);
            DIAG_IRQ(DT_IRQ_SV_STEPS, out_ch);
        } else {
            int32_t step = pitch_step(lp);
            if (pm || alias_rt || step <= 0) {
                for (unsigned f = from; f < to; f++) {
                    steps[f] = step;
                }
                DIAG_IRQ(DT_IRQ_SV_STEPS, out_ch);
            } else {
                DIAG_IRQ(DT_IRQ_SV_PLAY, out_ch);
                sr.per_frame = 0;
                sr.step = step;
                sr.kern = interp_kernel(ax[from], (uint32_t)step);
                gsteps = 0;
                gstep = step;
            }
        }
        if (gt || gright) {
            sv_render(v, &sr, pre, 0, to - from);
        } else if (out_ch == pc + 1 && (tl_right_ok >> out_ch) & 1) {
            tl_right(out_ch, v, &sr, pre, from, to - from);
        } else if (out_ch == pc) {
            tl_render(out_ch, v, &sr, pre, from, to - from);
        } else {
            sv_render(v, &sr, pre, 0, to - from);
        }
        v->trace = 0;
        if (gt) {
            unsigned was = prof_to(PS_GRN);
            grain_part(out_ch, gt, v, trace, gsteps, gstep, pre, from, to - from);
            prof_to(was);
        } else if (gright) {
            for (unsigned f = from; f < to; f++) {
                pre[f - from] = (grain_right_ok >> out_ch) & 1 ? grain_right[out_ch][f] : 0.0f;
            }
        }
        sample_bits(pre, to - from, bits);
        if ((pm_srcs >> out_ch) & 1) {
            mul_frames(chmod[chout_cur][out_ch] + from, pre, ev, to - from);
        }
        mul_frames(buf + from, buf + from, pre, to - from);
        if (ended) {
            v->on = 0;
        }
        engine_status.zone[out_ch] = v->zone;
        done = 1;
    }
    return done || v->on;
}

/* The channel envelope after a machine, as a gain per frame into ev, the
 * voice retriggered at frame off if trig (was_on: it was sounding).
 * Returns 0 if the gain is 1 over the whole block (ev untouched). */
static int machine_env(int out_ch, const struct sampler_cfg *s, int trig, unsigned off, int was_on,
                       float at_k, float rl_k, float *ev, unsigned frames, int *ended)
{
    struct svoice *v = &sv[out_ch];
    uint32_t held = fired_held(out_ch);
    int src = engine_status.trigger_src[out_ch];
    int releases = s->play_mode == PLAY_GATED || (src >= TRIG_SRC_LATCH && src < TRIG_SRC_AUTO);
    unsigned at = trig ? off : frames;

    *ended = 0;
    if (trig && at == 0) {
        v->stage = ENV_ATTACK;
    }
    if (v->stage == ENV_ATTACK && !(uint32_t)((float)s->attack * at_k) &&
        !(releases && held_run(held, 0, frames) < frames)) {
        v->env = 1.0f;
        return 0;
    }
    for (int part = 0; part < 2; part++) {
        unsigned from = part ? at : 0, to = part ? frames : at;
        if (part && trig) {
            if (!s->attack_from_current || !was_on) {
                v->env = 0.0f;
            }
            v->stage = ENV_ATTACK;
            *ended = 0;
        }
        if (from >= to) {
            continue;
        }
        unsigned f = from;
        for (; f < to && (f & 3); f++) {
            ev[f] = 1.0f;
        }
        for (; f + 4 <= to; f += 4) {
            vst1q_f32(ev + f, vdupq_n_f32(1.0f));
        }
        for (; f < to; f++) {
            ev[f] = 1.0f;
        }
        unsigned hold = held_run(held, from, to - from);
        *ended |= sv_env(v, s, part && trig && !hold ? to - from : hold, ev + from, 0, to - from, at_k, rl_k);
    }
    return 1;
}

/* Machine output through the sample interpolation kernels (Aliasing),
 * MDELAY frames behind the Hermite path's read; win holds MHIST frames of
 * history, then the block's. pos: per-frame Q16 positions, else frac + f * step. */
static void machine_alias(int out_ch, const float *win, const uint32_t *pos, uint32_t frac, uint32_t step,
                          int alias_x, const int16_t *ax, float *out, unsigned frames)
{
    int64_t q[AUDIO_BLOCK];
    uint16_t kk[AUDIO_BLOCK];
    int k0 = interp_kernel(alias_x, step << 9);

    k0 = k0 < INTERP_SHORT ? k0 : INTERP_SHORT;
    for (unsigned f = 0; f < frames; f++) {
        uint32_t p = pos ? pos[f] : frac + f * step;
        int k = ax ? interp_kernel(ax[f], step << 9) : k0;
        q[f] = ((int64_t)((p >> 16) + MHIST - 3 - MDELAY) << 32) | (int64_t)((p & 0xffffu) << 16);
        kk[f] = (uint16_t)(k < INTERP_SHORT ? k : INTERP_SHORT);
    }
    interp_run_f(out_ch, win, q, kk, out, frames);
}

/* buf times a machine's trim, through voice_clip (voices.h). */
static void machine_trim(float *buf, unsigned frames, float gain)
{
    const float32x4_t knee = vdupq_n_f32(0.5f), top = vdupq_n_f32(1.5f), zero = vdupq_n_f32(0.0f);
    const float32x4_t half = vdupq_n_f32(0.5f), g = vdupq_n_f32(gain);
    const uint32x4_t sign = vdupq_n_u32(0x80000000u);

    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t x = vmulq_f32(vld1q_f32(buf + f), g);
        float32x4_t a = vminq_f32(vabsq_f32(x), top);
        float32x4_t d = vmaxq_f32(vsubq_f32(a, knee), zero);
        uint32x4_t y = vreinterpretq_u32_f32(vmlsq_f32(a, vmulq_f32(d, d), half));
        vst1q_f32(buf + f, vreinterpretq_f32_u32(vorrq_u32(y, vandq_u32(vreinterpretq_u32_f32(x), sign))));
    }
}

static const struct channel_cfg *cfg_of(int ch)
{
    return (eff_on >> ch) & 1 ? &eff[ch] : &preset.ch[preset_param_channel(ch)];
}

/* Only the fields before the zones are read through cfg_of (zones are the
 * playing channel's own; EQ, filter, grain, Time Lock go by the master). */
#define EFF_WORDS (offsetof(struct channel_cfg, s.zone) / 4)
_Static_assert(offsetof(struct channel_cfg, s.zone) % 4 == 0, "word copy");

static void copy_words(void *d, const void *s, unsigned n)
{
    uint32_t *p = d;
    const uint32_t *q = s;

    while (n--) {
        *p++ = *q++;
    }
}

/* Stereo/Right channels with offsets: the master's parameters with them. */
static void pair_effective(void)
{
    eff_on = 0;
    for (int ch = 1; ch < CHANNELS; ch++) {
        const struct channel_cfg *rc = &preset.ch[ch];
        int pc;

        if (rc->mode != MODE_STEREO || !preset_has_offsets(rc) || (pc = preset_param_channel(ch)) == ch) {
            continue;
        }
        struct channel_cfg *e = &eff[ch];
        const int16_t *o = rc->ofs;
        int m = preset.ch[pc].machine != 0;

        copy_words(e, &preset.ch[pc], EFF_WORDS);
        e->pitch = (int16_t)preset_offset_apply(PO_PITCH, m, e->pitch, o[PO_PITCH]);
        e->level = (int16_t)preset_offset_apply(PO_LEVEL, m, e->level, o[PO_LEVEL]);
        e->mix = (int16_t)preset_offset_apply(PO_MIX, m, e->mix, o[PO_MIX]);
        e->bits = (int16_t)preset_offset_apply(PO_BITS, m, e->bits, o[PO_BITS]);
        if (o[PO_ATTACK]) {
            e->s.attack = env_time(preset_offset_apply(PO_ATTACK, m, env_index(e->s.attack), o[PO_ATTACK]));
        }
        if (o[PO_RELEASE]) {
            e->s.release = env_time(preset_offset_apply(PO_RELEASE, m, env_index(e->s.release), o[PO_RELEASE]));
        }
        e->s.aliasing = (uint8_t)preset_offset_apply(PO_ALIAS, m, e->s.aliasing, o[PO_ALIAS]);
        e->s.pm_index = (int16_t)preset_offset_apply(PO_PMINDEX, m, e->s.pm_index, o[PO_PMINDEX]);
        e->s.start_delay = (uint16_t)preset_offset_apply(PO_DELAY, m, e->s.start_delay, o[PO_DELAY]);
        for (int i = 0; i < CONTROLS; i++) {
            e->ctrl[i] = (uint8_t)preset_offset_apply(PO_CTRL + i, m, e->ctrl[i], o[PO_CTRL + i]);
        }
        eff_on |= (uint8_t)(1u << ch);
    }
}

#define CHUNK           32u     /* TRX and PI internal block */
#ifndef MACHINE_AHEAD
#define MACHINE_AHEAD   (AUDIO_BLOCK < 32)
#endif

/* n frames of a machine: first those rendered ahead, then the voice. */
static void machine_pull(struct rt *r, const mdkit_voice *v, float *dst, unsigned n)
{
    unsigned k = r->fifo_n < n ? r->fifo_n : n;

    if (k) {
        memcpy(dst, r->fifo, k * sizeof(float));
        r->fifo_n = (uint8_t)(r->fifo_n - k);
        memmove(r->fifo, r->fifo + k, r->fifo_n * sizeof(float));
    }
    if (n > k) {
        v->render(r->state, dst + k, n - k);
        r->phase = (uint8_t)((r->phase + n - k) % CHUNK);
    }
}

/* Internal blocks a render of n frames from phase p starts. */
static unsigned chunk_starts(unsigned p, unsigned n)
{
    unsigned first = (CHUNK - p) % CHUNK;

    return n > first ? (n - first - 1) / CHUNK + 1 : 0;
}

static void render_ahead(int ch)
{
    struct rt *r = &rt[ch];
    unsigned m = (CHUNK - r->phase) % CHUNK + 1u;

    mdkit_noise_salt = r->salt;
    voices[r->machine]->ops->render(r->state, r->fifo + r->fifo_n, m);
    r->fifo_n = (uint8_t)(r->fifo_n + m);
    r->phase = (uint8_t)((r->phase + m) % CHUNK);
    chunk_cost += r->cost;
#ifdef TRIG_TRACE
    chunk_n++;
#endif
#ifdef CHUNK_TRACE
    chunk_trace[block_now % 4096] |= (uint16_t)(256u << ch);
#endif
}

/* Chunked voices due to start an internal block next block: some start
 * it now (rendering ahead), costliest first, while that lowers the heavier
 * of the two blocks or keeps this one under the mean (by worst_us). A
 * trigger landing before that block's start then waits for the next. */
static void machines_ahead(void)
{
    int due[CHANNELS];
    unsigned nd = 0, next = 0, rate = 0;

    if (!MACHINE_AHEAD) {
        return;
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        const struct rt *r = &rt[ch];
        if (!r->chunked) {
            continue;
        }
        rate += r->cost * r->need;
        if (r->fifo_n + (CHUNK - r->phase) % CHUNK < r->need + 2u) {    /* next block's need may be one more */
            unsigned j = nd++;
            for (; j && rt[due[j - 1]].cost < r->cost; j--) {
                due[j] = due[j - 1];
            }
            due[j] = ch;
            next += r->cost;
        }
    }
    unsigned mean = rate / CHUNK;
    for (unsigned i = 0; i < nd; i++) {
        unsigned c = rt[due[i]].cost;
        if (chunk_cost + c < next || chunk_cost + c <= mean) {
            render_ahead(due[i]);
            next -= c;
        }
    }
}

/* Renders one channel into buf (48 kHz, post-level); returns 0 if silent,
 * -1 if its machine is still being prepared (a trigger then waits). A
 * trigger starts the voice `off` frames into the block. */
static int channel_render(int out_ch, int trig, unsigned off, float *buf, unsigned frames)
{
    int ch = preset_param_channel(out_ch);
    const struct channel_cfg *c = cfg_of(out_ch);
    struct rt *r = &rt[out_ch];
    const struct voice_desc *vd;
    const mdkit_voice *v;
    float win[MHIST + SRC_MAX + 4] __attribute__((aligned(16)));
    float *src = win + MHIST;

    if (r->ready) {
        uint8_t *old = r->state;
        r->state = r->spare;
        r->spare = old;
        r->machine = r->ready_machine;
        r->salt = r->ready_salt;
        memset(r->applied, 0xff, sizeof(r->applied));
        r->fifo_n = 0;
        r->phase = 0;
        r->ctl_ok = 0;
        sleep_voice(r);
        __asm__ volatile("dmb" ::: "memory");
        r->ready = 0;
    }
    if (c->machine != r->machine) {
        return -1;
    }
    vd = voices[r->machine];
    if (!vd) {
        DIAG_IRQ(DT_IRQ_SAMPLER, out_ch);
        prof_to(PS_SMP);
        return sampler_render(out_ch, ch, c, trig, off, buf, frames);
    }
    DIAG_IRQ(DT_IRQ_MACHINE, out_ch);
    prof_to(PS_MAC);
    sv[out_ch].on = 0;
    v = vd->ops;
    mdkit_noise_salt = r->salt;
    int pitch_in = vd->flags & VF_PITCH_IN;
    if (trig || !r->ctl_ok || ctl_block(out_ch)) {
        for (unsigned i = 0; i < vd->nparams && i < CONTROLS; i++) {
            int val = modulated(c, ch, D_CTRL + (int)i, c->ctrl[i], 0, 127);
            if (val != r->applied[i]) {
                v->set_control(r->state, i, (unsigned)val);
                r->applied[i] = (uint8_t)val;
            }
        }
        r->ctl_cents = modulated(c, ch, D_PITCH, c->pitch, -4800, pitch_in ? MPITCH_MAX : 2400);
        if (pitch_in) {
            voice_set_pitch(vd, r->state, (int32_t)((int64_t)r->ctl_cents * 65536 / 1200));
        }
        r->ctl_level = modulated(c, ch, D_LEVEL, c->level, LEVEL_MIN, LEVEL_MAX);
        r->ctl_bits = modulated(c, ch, D_BITS, c->bits, BITS_MIN, BITS_MAX);
        r->ctl_at_k = c->s.mod[SM_ATTACK].src ? fast_exp2(smod(&c->s, SM_ATTACK, ch)) : 1.0f;
        r->ctl_rl_k = c->s.mod[SM_RELEASE].src ? fast_exp2(smod(&c->s, SM_RELEASE, ch)) : 1.0f;
        r->ctl_ok = 1;
        sctl[out_ch].ok = 0;
    }
    int cents = r->ctl_cents;
    int was_on = !r->asleep;
    if (trig) {
        if (!off) {
            v->trigger(r->state);
        }
        wake(r);
        r->fade = 0;
    }
    int level = r->ctl_level, bits = r->ctl_bits;
    if (r->asleep) {
        return 0;
    }

    if (!r->state) {
        return 0;
    }
    int skey = pitch_in ? INT32_MIN : cents;
    if (!(r->memo & 1) || r->step_key != skey) {
        float stepf = (VOICE_RATE / AUDIO_RATE) * (pitch_in ? 1.0f : fast_exp2((float)cents * (1.0f / 1200.0f)));
        uint32_t st = (uint32_t)(stepf * 65536.0f);
        r->step_val = st > MAX_STEP ? MAX_STEP : st < 1 ? 1 : st;
        r->step_key = skey;
        r->memo |= 1;
    }
    uint32_t step = r->step_val;
    /* Pitch CV changes ramp the step over the block (no zipper). */
    uint32_t s0 = trig || !r->step ? step : r->step, pos[AUDIO_BLOCK];
    uint32_t end;
    if (s0 != step) {
        int32_t d = (int32_t)(step - s0);
        uint32_t p = r->frac;
        for (unsigned f = 0; f < frames; f++) {
            pos[f] = p;
            p += s0 + (uint32_t)(d * (int32_t)(f + 1) / (int32_t)frames);
        }
        end = p;
    } else {
        end = r->frac + step * frames;
    }
    r->step = step;
    unsigned need = end >> 16;
    float peak;

    int chunked = vd->family == FAM_TRX || vd->family == FAM_PI;
    if (chunked) {
        unsigned avail = r->fifo_n + (CHUNK - r->phase) % CHUNK;
        chunk_cost += need > avail ? chunk_starts(r->phase, need - r->fifo_n) * vd->worst_us : 0u;
#ifdef TRIG_TRACE
        chunk_n += need > avail ? chunk_starts(r->phase, need - r->fifo_n) : 0u;
#endif
#ifdef CHUNK_TRACE
        chunk_trace[block_now % 4096] |= need > avail ? (uint16_t)(1u << out_ch) : 0u;
#endif
        r->cost = vd->worst_us;
    }
#ifdef TRIG_TRACE
    if (trig && chunked) {
        unsigned at = off ? (r->frac + off * step) >> 16 : 0u;
        at = at < need ? at : need;
        trig_trace[0][vd->family]++;
        trig_trace[1][vd->family] += r->fifo_n >= at + (r->phase ? r->phase : CHUNK);
    }
#endif
    if (trig && off) {
        unsigned at = (r->frac + off * step) >> 16;
        at = at < need ? at : need;
        machine_pull(r, v, src, at);
        v->trigger(r->state);
        machine_pull(r, v, src + at, need - at);
    } else {
        machine_pull(r, v, src, need);
    }
    r->chunked = (uint8_t)chunked;
    r->need = (uint8_t)need;
    peak = need ? peak_abs(src, need) * vd->gain : 0.0f;
    memcpy(win, r->hist, sizeof(r->hist));
    /* AliasingMod: as on sample channels. Aliasing 0 without a route keeps
     * the Hermite resampler. */
    const struct sampler_cfg *s = &c->s;
    const float *avf = route_frames(s->mod[SM_ALIAS].src, ch);
    int alias_rt = avf && s->mod[SM_ALIAS].amt;
    if (s->aliasing || alias_rt) {
        int16_t ax[AUDIO_BLOCK];
        int alias_x = interp_alias_x(s->aliasing);
        float ak = 13107.2f * (float)s->mod[SM_ALIAS].amt * 0.01f;
        for (unsigned f = 0; alias_rt && f < frames; f++) {
            float x = (float)alias_x + ak * avf[f];
            ax[f] = (int16_t)(x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int)(x + (x < 0 ? -0.5f : 0.5f)));
        }
        machine_alias(out_ch, win, s0 != step ? pos : 0, r->frac, step, alias_x, alias_rt ? ax : 0, buf, frames);
    } else if (s0 != step) {
        resample_pos(win + MHIST - 4, pos, buf, frames);
    } else {
        resample(win + MHIST - 4, r->frac, step, buf, frames);
    }
    memcpy(r->hist, win + need, sizeof(r->hist));
    r->frac = end & 0xffffu;
    machine_trim(buf, frames, vd->gain);
    float ev[AUDIO_BLOCK] __attribute__((aligned(16)));
    int ended, env = machine_env(out_ch, s, trig, off, was_on, r->ctl_at_k, r->ctl_rl_k, ev, frames, &ended);
    if ((pm_srcs >> out_ch) & 1) {
        float *m = chmod[chout_cur][out_ch];
        for (unsigned f = 0; f < frames; f++) {
            m[f] = env ? buf[f] * ev[f] : buf[f];
        }
    }

    if (!(r->memo & 2) || r->level_key != level) {
        r->level_val = db_gain(level);
        r->level_key = level;
        r->memo |= 2;
    }
    float gain = r->level_val;
    if (bits < BITS_MAX) {
        if (!(r->memo & 4) || r->bits_key != bits) {
            r->bits_q = bits_levels(bits);
            r->bits_iq = 1.0f / r->bits_q;
            r->bits_key = bits;
            r->memo |= 4;
        }
        apply_bits(buf, frames, r->bits_q, r->bits_iq);
    }
    if (env) {
        for (unsigned f = 0; f < frames; f += 4) {
            vst1q_f32(buf + f, vmulq_f32(vld1q_f32(buf + f), vld1q_f32(ev + f)));
        }
    }
    gain_ramp(buf, frames, trig || r->gain < 0.0f ? gain : r->gain, gain);
    r->gain = gain;
    if (r->fade) {
        float g = (float)r->fade * (1.0f / FADE_BLOCKS), dg = 1.0f / (FADE_BLOCKS * (float)frames);
        for (unsigned f = 0; f < frames; f++) {
            buf[f] *= g - dg * (float)f;
        }
        if (!--r->fade) {
            sleep_voice(r);
        }
    }
    if (ended) {
        sleep_voice(r);
        return 1;
    }

    if (peak < QUIET_LEVEL) {
        if (++r->quiet > QUIET_BLOCKS) {
            sleep_voice(r);
        }
    } else {
        r->quiet = 0;
    }
    return 1;
}

/* Mix level after MixMod; sample channels stop at stock's +6 dB
 * (measured). Fader mode (stock, measured): the CV is biased down 5 V, so
 * 0 V is -50 dB at 1.00 and +5 V the set level. */
static int mix_level(const struct channel_cfg *c, int ch)
{
    const struct mod *m = &c->mod[D_MIX];
    int hi = c->machine ? LEVEL_MAX : SLEVEL_MAX;

    if (c->s.mixmod_fader && m->src != CV_OFF) {
        float dv = (engine_cv(m->src, ch) - 5.0f) * per_volt(D_MIX) * (float)m->amt * 0.01f;
        int v = clampi(c->mix + (int)(dv + (dv < 0 ? -0.5f : 0.5f)), MIX_MIN, hi);
        engine_value[ch][D_MIX] = (int16_t)v;
        return v;
    }
    return modulated(c, ch, D_MIX, c->mix, MIX_MIN, hi);
}

/* Crossfade groups (stock, measured): the members in channel order split
 * -5..+5 V evenly, positive toward the lowest channel; each hands over to
 * the next over the group's width centred on their boundary, fading in as
 * x(2 - x) (0.75 each at the boundary). g[f] per frame; 0 if ch is in no
 * group. A group without a CV sits at 0 V. */
#define XF_SPAN 10.0f

static int xfade_gains(int ch, float *g, unsigned frames)
{
    int grp = preset.ch[ch].s.xfade, n = 0, k = 0;

    if (!grp) {
        return 0;
    }
    for (int c = 0; c < CHANNELS; c++) {
        if (preset.ch[c].s.xfade == grp) {
            k = c == ch ? n : k;
            n++;
        }
    }
    int in = preset.xfade_cv[grp - 1];
    const float *v = in ? cv_frames(in - 1) : 0;
    float w = (float)preset.xfade_width[grp - 1] * 0.01f, iw = 1.0f / w;
    float hi = 5.0f - XF_SPAN * (float)k / (float)n, lo = 5.0f - XF_SPAN * (float)(k + 1) / (float)n;
    const float32x4_t zero = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f), two = vdupq_n_f32(2.0f);
    const float32x4_t ah = vdupq_n_f32(k ? hi * iw + 0.5f : 2.0f), bl = vdupq_n_f32(k < n - 1 ? 0.5f - lo * iw : 2.0f);
    const float32x4_t ka = vdupq_n_f32(k ? iw : 0.0f), kb = vdupq_n_f32(k < n - 1 ? iw : 0.0f);
    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t x = v ? vld1q_f32(v + f) : zero;
        float32x4_t a = vminq_f32(vmaxq_f32(vmlsq_f32(ah, x, ka), zero), one);
        float32x4_t b = vminq_f32(vmaxq_f32(vmlaq_f32(bl, x, kb), zero), one);
        a = vmulq_f32(a, vsubq_f32(two, a));
        b = vmulq_f32(b, vsubq_f32(two, b));
        vst1q_f32(g + f, vmulq_f32(a, b));
    }
    return 1;
}

/* Channel ch's phase mod source channel, or -1 (none, or itself). */
static int pm_dep(int ch)
{
    const struct channel_cfg *c = &preset.ch[preset_param_channel(ch)];
    int src = c->s.pm_source;

    if (c->machine || src >= CHANNELS || src == ch || (!c->s.pm_index && !c->s.mod[SM_PMINDEX].src)) {
        return -1;
    }
    return src;
}

/* Phase mod sources render before their carriers; a cycle takes the last
 * block of the channel that closes it. */
static void render_order(int *order)
{
    uint8_t state[CHANNELS] = { 0 };
    int n = 0;

    pm_srcs = 0;
    for (int ch = 0; ch < CHANNELS; ch++) {
        int d = pm_dep(ch);
        pm_srcs |= (uint8_t)(d >= 0 ? 1u << d : 0u);
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        int stack[CHANNELS], sp = 0;

        if (state[ch]) {
            continue;
        }
        stack[sp++] = ch;
        state[ch] = 1;
        while (sp) {
            int c = stack[sp - 1], d = pm_dep(c);
            if (d < 0 || state[d]) {
                d = grain_dep(c);
            }
            if (d >= 0 && !state[d]) {
                state[d] = 1;
                stack[sp++] = d;
                continue;
            }
            state[c] = 2;
            order[n++] = c;
            sp--;
        }
    }
}

#define PEAK_DECAY      BLOCK_DECAY(0.97f)  /* meters */

static float decay_peak(float old, float now)
{
    old *= PEAK_DECAY;
    return now > old ? now : old;
}

#ifdef TEST_FAULT
/* See main.c: 2 data abort (unaligned LDM), 3 spin, 5 data abort during
 * an OLED flush, 7 a trap, all in the audio IRQ. */
static void test_fault_irq(void)
{
    static const uint32_t words[3] __attribute__((aligned(4))) = { 0 };
    uint32_t a = (uint32_t)words + 1, x, y;

    if (time_us() < 3000000u) {
        return;
    }
    if (TEST_FAULT == 5 && (diag_main_tag >> 16) != DT_OLED_FLUSH) {
        return;
    }
    switch (TEST_FAULT) {
    case 2:
    case 5:
        __asm__ volatile("ldm %2, {%0, %1}" : "=r"(x), "=r"(y) : "r"(a) : "memory");
        break;
    case 3:
        for (;;) {
        }
    case 7:
        DIAG_TRAP(TRAP_TEST);
        break;
    }
}
#endif

/* Timestamped gate edges up to when the block before this one started
 * playing (audio_block_ticks), each at its frame in this block: gate_held,
 * the first rise per gate and its frame. Returns the levels at the end. */
static uint8_t gate_edges(unsigned frames, uint8_t *rising, uint8_t *rise_at)
{
    static uint8_t level;
    static int started;
    const uint32_t tpf = AUDIO_TICKS_PER_FRAME, all = frames < 32 ? (1u << frames) - 1u : ~0u;
    uint32_t end = audio_block_ticks(), start = end - frames * tpf;
    const struct gate_edge *e;

    if (!started || panel_gate_edge_lost()) {
        started = 1;
        level = panel_gates_now();
    }
    for (int ch = 0; ch < CHANNELS; ch++) {
        gate_held[ch] = (level >> ch) & 1 ? all : 0u;
    }
    while ((e = panel_gate_edge_peek()) && (int32_t)(e->t - end) < 0) {
        int32_t d = (int32_t)(e->t - start);
        unsigned f = d <= 0 ? 0 : (unsigned)d / tpf;
        uint32_t later = all & ~((1u << f) - 1u);
        uint8_t b = (uint8_t)(1u << e->ch);
        if (e->on) {
            gate_held[e->ch] |= later;
            if (!(*rising & b)) {
                *rising |= b;
                rise_at[e->ch] = (uint8_t)f;
            }
            level |= b;
        } else {
            gate_held[e->ch] &= ~later;
            level &= (uint8_t)~b;
        }
        panel_gate_edge_pop();
    }
    return level;
}

/* START DELAY of a channel in frames, with its CV (100 ms/V at 1.00). */
static unsigned start_delay_frames(int ch)
{
    const struct channel_cfg *c = cfg_of(ch);
    const struct mod *m = &c->s.mod[SM_DELAY];
    int t = c->s.start_delay;

    if (m->src != CV_OFF) {
        float dv = engine_cv(m->src, preset_param_channel(ch)) * 1000.0f * (float)m->amt * 0.01f;
        t = clampi(t + (int)(dv + (dv < 0 ? -0.5f : 0.5f)), 0, SDELAY_MAX);
    }
    return ((unsigned)t * (AUDIO_RATE / 1000u) + 5u) / 10u;
}

/* Triggers through each channel's START DELAY: a delayed one waits in the
 * channel's queue, behind any already waiting (so the order is kept);
 * returns the channels firing in this block (off, src: their frame and
 * source). */
static uint8_t start_delays(uint8_t fire, uint8_t *off, uint8_t *src, unsigned frames)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        struct sd_queue *q = &sdq[ch];
        uint8_t b = (uint8_t)(1u << ch);

        if (fire & b) {
            unsigned d = start_delay_frames(ch);
            if (!d && !q->n) {
                note_delay[ch] = 0;
                continue;
            }
            fire &= (uint8_t)~b;
            if (q->n < SD_QUEUE) {
                unsigned i = (q->head + q->n++) % SD_QUEUE;
                q->at[i] = frame_now + off[ch] + d;
                q->delay[i] = (uint16_t)d;
                q->src[i] = src[ch];
            }
        }
        if (q->n && (int32_t)(q->at[q->head] - (frame_now + frames)) < 0) {
            int32_t at = (int32_t)(q->at[q->head] - frame_now);
            off[ch] = (uint8_t)(at < 0 ? 0 : at);
            src[ch] = q->src[q->head];
            note_delay[ch] = q->delay[q->head];
            q->head = (uint8_t)((q->head + 1) % SD_QUEUE);
            q->n--;
            fire |= b;
        }
    }
    return fire;
}

/* A channel's send source for fx_mix_render, Q31: the channel at a level
 * ramping g0 -> g over the block (Pre: 1). */
static void send_source(const float *buf, int ch, float g0, float g, unsigned frames)
{
    const float32x4_t i4 = { 1.0f / (float)frames, 2.0f / (float)frames, 3.0f / (float)frames,
                             4.0f / (float)frames };
    const float32x4_t step4 = vdupq_n_f32(4.0f / (float)frames);
    float32x4_t t = i4;

    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t gv = vmlaq_n_f32(vdupq_n_f32(g0), t, g - g0);
        int32x4_t w = vcvtq_s32_f32(vmulq_n_f32(vmulq_f32(vld1q_f32(buf + f), gv), 2147483648.0f));
        vst1q_lane_s32(&sends[f][ch], w, 0);
        vst1q_lane_s32(&sends[f + 1][ch], w, 1);
        vst1q_lane_s32(&sends[f + 2][ch], w, 2);
        vst1q_lane_s32(&sends[f + 3][ch], w, 3);
        t = vaddq_f32(t, step4);
    }
}

/* The mix through the sends' returns and the mix compressor, in Q31. */
static void mix_effects(float *ml, float *mr, unsigned flags, unsigned frames)
{
    int32_t m[AUDIO_BLOCK][2] __attribute__((aligned(16)));

    for (unsigned f = 0; f < frames; f += 4) {
        int32x4x2_t w = { { vcvtq_s32_f32(vmulq_n_f32(vld1q_f32(ml + f), 2147483648.0f)),
                            vcvtq_s32_f32(vmulq_n_f32(vld1q_f32(mr + f), 2147483648.0f)) } };
        vst2q_s32(&m[f][0], w);
    }
    fx_mix_render(m, (const int32_t (*)[CHANNELS])sends, frames, flags);
    for (unsigned f = 0; f < frames; f += 4) {
        int32x4x2_t w = vld2q_s32(&m[f][0]);
        vst1q_f32(ml + f, vmulq_n_f32(vcvtq_f32_s32(w.val[0]), 1.0f / 2147483648.0f));
        vst1q_f32(mr + f, vmulq_n_f32(vcvtq_f32_s32(w.val[1]), 1.0f / 2147483648.0f));
    }
}

/* Every voice stopped where it is (the overload guard, a hush). */
static void stop_voices(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        sleep_voice(&rt[ch]);
        rt[ch].fade = 0;
        sv[ch].on = 0;
        grain_rest(ch);
        tlock_idle(ch);
        sdq[ch].n = 0;
    }
    grain_live = 0;
}

static void hush_done(void)
{
    stop_voices();
    memset(fx_tail, 0, sizeof(fx_tail));
    mix_live = 0;
    hush_fade = 0;
    engine_status.awake = 0;
    engine_status.latched = 0;
    /* a channel's chout is a phase mod source next block */
    memset(chout, 0, sizeof(chout));
    memset(chmod, 0, sizeof(chmod));
    __asm__ volatile("dmb" ::: "memory");
    hushed = 1;
}

void engine_hush(void)
{
    uint32_t t0 = time_us(), b0 = audio_blocks();

    hush_req = 1;
    while (!hushed) {
        uint32_t t = time_us() - t0;
        if (!b0 || t > 50000u || (t > 5000u && audio_blocks() == b0)) {
            /* audio isn't running */
            uint32_t f;
            __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(f) :: "memory");
            if (!hushed) {
                hush_done();
            }
            __asm__ volatile("msr cpsr_c, %0" :: "r"(f) : "memory");
        }
    }
}

void engine_unhush(void)
{
    __asm__ volatile("dmb" ::: "memory");
    hush_req = 0;
}

/* The block's outputs scaled by a ramp g0 -> g1. */
static void hush_ramp(int32_t out[][AUDIO_OUTPUTS], int32_t mix[][2], unsigned frames, float g0, float g1)
{
    float d = (g1 - g0) / (float)frames;

    for (unsigned f = 0; f < frames; f++) {
        float g = g0 + d * (float)(f + 1);
        for (unsigned o = 0; o < AUDIO_OUTPUTS; o++) {
            out[f][o] = (int32_t)((float)out[f][o] * g);
        }
        mix[f][0] = (int32_t)((float)mix[f][0] * g);
        mix[f][1] = (int32_t)((float)mix[f][1] * g);
    }
}

void engine_render(int32_t out[][AUDIO_OUTPUTS], int32_t mix[][2],
                   const int32_t in[][2], unsigned frames)
{
    uint32_t start = time_ticks();
    DIAG_IRQ(DT_IRQ_CV, 0);
    interp_block();
#ifdef TEST_FAULT
    test_fault_irq();
#endif
    float ml[AUDIO_BLOCK] __attribute__((aligned(16))), mr[AUDIO_BLOCK] __attribute__((aligned(16)));
    uint8_t gates, rising = 0, rise_at[CHANNELS] = { 0 };
    prof_to(PS_RGT);
    if (panel_gate_irq()) {
        gates = gate_edges(frames, &rising, rise_at);
    } else {
        gates = panel_gates_now();
        uint8_t level_rise = gates & (uint8_t)~last_gates, edges = panel_gate_edges();
        /* Triggers come from the latched edges (short pulses between blocks
         * count, no double counting); if the edge latch never fires while gate
         * levels do rise, fall back to the levels. */
        static unsigned level_only_rises;
        static int use_levels, seen_edges;
        if (edges) {
            seen_edges = 1;
        } else if (level_rise && !seen_edges && ++level_only_rises >= 4) {
            use_levels = 1;
        }
        rising = use_levels ? level_rise : edges;
        for (int ch = 0; ch < CHANNELS; ch++) {
            gate_held[ch] = (gates >> ch) & 1 ? ~0u : 0u;
        }
    }
    uint8_t awake = 0;

    last_gates = gates;
    engine_status.gates = gates;
    panel_sample();
    prof_to(PS_RCV);
    cv_poll();
    preset_cv_scan();
    prof_to(PS_RGT);
    DIAG_IRQ(DT_IRQ_TRIGGERS, 0);
    uint8_t fire = 0, latched = engine_status.latched;
    uint8_t off[CHANNELS] = { 0 }, tsrc[CHANNELS] = { 0 };
    unsigned salt0 = mdkit_noise_salt;
    pair_effective();
    for (int ch = 0; ch < CHANNELS; ch++) {
        uint8_t group = 0, src = 0, b = (uint8_t)(1u << ch);
        unsigned at = 0;
        if (((rising & ~latched) >> ch) & 1 && !gate_wait[ch]) {
            gate_wait[ch] = (uint16_t)(gate_delay + rise_at[ch] + 1);
        }
        if (gate_wait[ch]) {
            if (gate_wait[ch] <= frames) {
                at = gate_wait[ch] - 1u;
                gate_wait[ch] = 0;
                if (!(latched & b)) {
                    group = gate_group(ch) & (uint8_t)~latched;
                    src = (uint8_t)ch;
                }
            } else {
                gate_wait[ch] = (uint16_t)(gate_wait[ch] - frames);
            }
        }
        if (latch_req[ch]) {
            latch_req[ch] = 0;
            if (latched & b) {
                latched &= (uint8_t)~b;
                for (int c = 0; c < CHANNELS; c++) {
                    if (engine_status.trigger_src[c] == TRIG_SRC_LATCH + ch && !rt[c].asleep) {
                        rt[c].fade = FADE_BLOCKS;
                    }
                }
            } else {
                latched |= b;
                ui_trig[ch] = 1;
            }
        }
        if (ui_trig[ch] || auto_trig[ch]) {
            src = (uint8_t)(ui_trig[ch] ? 8 + ch : TRIG_SRC_AUTO);
            if (latched & b) {
                src = (uint8_t)(TRIG_SRC_LATCH + ch);
            }
            ui_trig[ch] = auto_trig[ch] = 0;
            group = is_master(ch) ? gate_group(ch) : b;
            at = 0;
        }
        for (int c = 0; group >> c; c++) {
            if ((group >> c) & 1) {
                tsrc[c] = src;
                off[c] = (uint8_t)at;
            }
        }
        fire |= group;
    }
    /* After an overload trip a running sequencer would restart the one-shot
     * loops (grains) at once, and they would play on once the heavy voices
     * have died away. */
    if (overload_hold) {
        overload_hold--;
        fire = 0;
        latched = 0;
        deferred = 0;
        for (int c = 0; c < CHANNELS; c++) {
            sdq[c].n = 0;
        }
    }
    engine_status.latched = latched;
    {
        struct held_hist *h = &held_hist[block_now % HELD_BLOCKS];
        memcpy(h->gate, gate_held, sizeof(h->gate));
        h->buttons = (uint8_t)(engine_buttons | engine_virtual_buttons);
        h->latched = latched;
    }
    fire = start_delays(fire, off, tsrc, frames);
    for (int c = 0; c < CHANNELS; c++) {
        if ((fire >> c) & 1) {
            engine_status.trigger_src[c] = tsrc[c];
        }
    }
    fire |= deferred;
    prof_to(PS_REST);
    if (hushed && !hush_req) {
        hushed = 0;
        fire |= hush_fire;
        hush_fire = 0;
    }
    if (hush_req) {
        if (!hushed && !hush_fade) {
            hush_fade = HUSH_BLOCKS;
        }
        hush_fire |= fire;
        fire = 0;
    }
    if (hushed) {
        memset(out, 0, sizeof(out[0]) * frames);
        memset(mix, 0, sizeof(mix[0]) * frames);
        for (int ch = 0; ch < CHANNELS; ch++) {
            engine_status.peak[ch] *= PEAK_DECAY;
        }
        overload_run = 0;
        frame_now += frames;
        block_now++;
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        ml[f] = mr[f] = 0.0f;
    }
    int order[CHANNELS];
    render_order(order);
    sample_in = in;
    chout_cur ^= 1u;
    chout_done = 0;
    grain_right_ok = 0;
    tl_right_ok = 0;
    prof_to(PS_TL);
    tlock_block(frames, in);
    prof_to(PS_REST);
    uint8_t live = 0, lanes, comp_lanes;
    int fx = fx_busy(&lanes);
    unsigned mixfx = fx_mix_busy(frames, &comp_lanes);
    if (mixfx & FXM_EFFECTS) {
        memset(sends, 0, sizeof(sends));
    }
    for (int i = 0; i < CHANNELS; i++) {
        int ch = order[i];
        int trig = (fire >> ch) & 1, ok;
        float *buf = chout[chout_cur][ch];
        uint8_t b = (uint8_t)(1u << ch);
        if ((pm_srcs >> ch) & 1) {
            memset(chmod[chout_cur][ch], 0, sizeof(chmod[0][0]));
        }

        vslot = preset_param_channel(ch) != ch ? ch : -1;
        rt[ch].chunked = 0;
        ok = channel_render(ch, trig, off[ch], buf, frames);
        vslot = -1;
        if (trig) {
            if (ok < 0 && ++defer_age[ch] < DEFER_BLOCKS) {
                deferred |= b;
            } else {
                deferred &= (uint8_t)~b;
                defer_age[ch] = 0;
                engine_status.triggers[ch]++;
            }
        }
        chout_done |= b;
        if (ok <= 0) {
            for (unsigned f = 0; f < frames; f++) {
                buf[f] = 0.0f;
            }
            if (((lanes >> ch) & 1) && fx_tail[ch]) {
                live |= b;
            }
            continue;
        }
        {
            float xg[AUDIO_BLOCK] __attribute__((aligned(16)));
            if (xfade_gains(preset_param_channel(ch), xg, frames)) {
                for (unsigned f = 0; f < frames; f += 4) {
                    vst1q_f32(buf + f, vmulq_f32(vld1q_f32(buf + f), vld1q_f32(xg + f)));
                }
            }
        }
        live |= b;
        awake |= b;
        fx_tail[ch] = FX_TAIL_BLOCKS;
    }
    prof_to(PS_MAC);
    machines_ahead();
    chunk_cost = 0;
#ifdef TRIG_TRACE
    chunk_hist[chunk_n < 16 ? chunk_n : 16]++;
    if (block_now & 1) {
        chunk_span[chunk_n + chunk_prev < 32 ? chunk_n + chunk_prev : 32]++;
    }
    chunk_prev = chunk_n;
    chunk_n = 0;
#endif
    mdkit_noise_salt = salt0;
    prof_to(PS_CFX);
    if (fx || (mixfx & FXM_CHANNELS)) {
        DIAG_IRQ(DT_IRQ_FX, 0);
        fx_render(chout[chout_cur], frames, fx, lanes | comp_lanes, mixfx, in);
    }
    if (fx) {
        for (int ch = 0; ch < CHANNELS; ch++) {
            if (((live & ~awake) >> ch) & 1 && peak_abs(chout[chout_cur][ch], frames) < QUIET_LEVEL) {
                fx_tail[ch]--;
            }
        }
    }
    prof_to(PS_MIX);
    const float32x4_t step4 = vdupq_n_f32(4.0f / (float)frames);
    const float32x4_t i4 = { 1.0f / (float)frames, 2.0f / (float)frames, 3.0f / (float)frames,
                             4.0f / (float)frames };
    for (int i = 0; i < CHANNELS; i++) {
        int ch = order[i];
        const struct channel_cfg *c = cfg_of(ch);
        float peak = 0.0f;
        float *buf = chout[chout_cur][ch];

        if (!((live >> ch) & 1)) {
            mix_live &= (uint8_t)~(1u << ch);
            for (unsigned f = 0; f < frames; f++) {
                out[f][ch] = 0;
            }
            if (ch == engine_scope_ch) {
                scope_capture(buf, frames);
            }
            engine_status.peak[ch] *= PEAK_DECAY;
            continue;
        }
        int was_live = (mix_live >> ch) & 1;
        float gl = mix_gl[ch], gr = mix_gr[ch], mg = mix_g[ch];
        if (!was_live || ((fire >> ch) & 1) || ctl_block(ch)) {
            /* A stereo pair (stock, measured): the first channel hard left, the
             * Stereo/Right one hard right, Pan ignored. */
            int pan = preset.ch[ch].mode == MODE_STEREO ? 100 :
                      ch + 1 < CHANNELS && preset.ch[ch + 1].mode == MODE_STEREO ? -100 :
                      modulated(&preset.ch[ch], ch, D_PAN, preset.ch[ch].pan, -100, 100);
            int mixdb = mix_level(c, ch);
            /* Stock's pan law (measured): g(x) = x(2 - x) per side, -2.5 dB at
             * the centre. */
            float p = (float)(pan + 100) * 0.005f, q = 1.0f - p;
            gr = p * (1.0f + q);
            gl = q * (1.0f + p);
            if (!((mix_memo >> ch) & 1) || mix_key[ch] != mixdb) {
                mix_val[ch] = fast_exp2((float)mixdb * (0.1f / 6.0205999f));
                mix_key[ch] = mixdb;
                mix_memo |= (uint8_t)(1u << ch);
            }
            mg = c->mix_on && mixdb > MIX_MIN ? mix_val[ch] : 0.0f;
            gl *= mg;
            gr *= mg;
        }
        /* Pan/mix CV changes ramp over the block. */
        float gl0 = was_live ? mix_gl[ch] : gl, gr0 = was_live ? mix_gr[ch] : gr;
        float mg0 = was_live ? mix_g[ch] : mg;
        float32x4_t t = i4;
        mix_gl[ch] = gl;
        mix_gr[ch] = gr;
        mix_g[ch] = mg;
        if (mixfx & FXM_EFFECTS) {
            send_source(buf, ch, fx_send_pre(ch) ? 1.0f : mg0, fx_send_pre(ch) ? 1.0f : mg, frames);
        }
        mix_live |= (uint8_t)(1u << ch);
        float32x4_t pk = vdupq_n_f32(0.0f);
        for (unsigned f = 0; f < frames; f += 4) {
            float32x4_t s = vld1q_f32(buf + f);
            int32x4_t w = vcvtq_s32_f32(vmulq_n_f32(s, 2147483648.0f));    /* saturates */
            vst1q_lane_s32(&out[f][ch], w, 0);
            vst1q_lane_s32(&out[f + 1][ch], w, 1);
            vst1q_lane_s32(&out[f + 2][ch], w, 2);
            vst1q_lane_s32(&out[f + 3][ch], w, 3);
            float32x4_t l = vmlaq_n_f32(vdupq_n_f32(gl0), t, gl - gl0), r = vmlaq_n_f32(vdupq_n_f32(gr0), t, gr - gr0);
            vst1q_f32(ml + f, vmlaq_f32(vld1q_f32(ml + f), s, l));
            vst1q_f32(mr + f, vmlaq_f32(vld1q_f32(mr + f), s, r));
            t = vaddq_f32(t, step4);
            pk = vmaxq_f32(pk, vabsq_f32(s));
        }
        float32x2_t ph = vpmax_f32(vget_low_f32(pk), vget_high_f32(pk));
        peak = vget_lane_f32(vpmax_f32(ph, ph), 0);
        engine_status.peak[ch] = decay_peak(engine_status.peak[ch], peak * 2.0f);
        if (ch == engine_scope_ch) {
            scope_capture(buf, frames);
        }
    }
    DIAG_IRQ(DT_IRQ_MIX, 0);
    if (mixfx & (FXM_MIX | FXM_EFFECTS)) {
        mix_effects(ml, mr, mixfx, frames);
    }
    float32x4_t pkl = vdupq_n_f32(0), pkr = pkl;
    int32x4_t pki = vdupq_n_s32(0);
    for (unsigned f = 0; f < frames; f += 4) {
        float32x4_t l = vld1q_f32(ml + f), r = vld1q_f32(mr + f);
        int32x4x2_t w = { { vcvtq_s32_f32(vmulq_n_f32(l, 2147483648.0f)),
                            vcvtq_s32_f32(vmulq_n_f32(r, 2147483648.0f)) } };
        vst2q_s32(&mix[f][0], w);                       /* interleaves L/R */
        pkl = vmaxq_f32(pkl, vabsq_f32(l));
        pkr = vmaxq_f32(pkr, vabsq_f32(r));
        pki = vmaxq_s32(pki, vqabsq_s32(vld1q_s32(&in[f][0])));
        pki = vmaxq_s32(pki, vqabsq_s32(vld1q_s32(&in[f + 2][0])));
    }
    float32x2_t hl = vpmax_f32(vget_low_f32(pkl), vget_high_f32(pkl));
    float32x2_t hr = vpmax_f32(vget_low_f32(pkr), vget_high_f32(pkr));
    float pl = vget_lane_f32(vpmax_f32(hl, hl), 0), pr = vget_lane_f32(vpmax_f32(hr, hr), 0);
    int32x2_t hi = vmax_s32(vget_low_s32(pki), vget_high_s32(pki));   /* lanes: L, R */
    float il = (float)vget_lane_s32(hi, 0) * (1.0f / 2147483648.0f);
    float ir = (float)vget_lane_s32(hi, 1) * (1.0f / 2147483648.0f);
    engine_status.mix_peak[0] = decay_peak(engine_status.mix_peak[0], pl);
    engine_status.mix_peak[1] = decay_peak(engine_status.mix_peak[1], pr);
    engine_status.in_peak[0] = decay_peak(engine_status.in_peak[0], il);
    engine_status.in_peak[1] = decay_peak(engine_status.in_peak[1], ir);
    engine_status.awake = awake;
    frame_now += frames;
    block_now++;
    prof_to(PS_REST);
    if (hush_fade) {
        hush_ramp(out, mix, frames, (float)hush_fade / HUSH_BLOCKS, (float)(hush_fade - 1) / HUSH_BLOCKS);
        if (!--hush_fade) {
            hush_done();
        }
    }

    /* Render time and the overload guard per 32 frames (667 us). */
    static uint32_t win_ticks;
    static unsigned win_n;
    win_ticks += time_ticks() - start;
    if (++win_n < BLOCKS_PER_32) {
        return;
    }
    uint32_t us = win_ticks / 24u;
    win_ticks = 0;
    win_n = 0;
    engine_status.render_us_last = us;
    if (us > engine_status.render_us_max) {
        engine_status.render_us_max = us;
    }
    /* Sustained overload starves the main loop (and the UI): silence
     * everything until the next trigger rather than lock up. */
#ifdef TEST_OVERLOAD
    /* EXTRA=-DTEST_OVERLOAD=s: OVERLOAD_BLOCKS blocks over budget s seconds
     * after channel 1's first trigger. */
    static unsigned forced;
    static uint32_t first;
    if (!first && engine_status.triggers[0]) {
        first = time_us() | 1u;
    }
    if (first && forced < OVERLOAD_BLOCKS && time_us() - first > TEST_OVERLOAD * 1000000u) {
        forced++;
        us = OVERLOAD_US + 1u;
    }
#endif
    if (us > OVERLOAD_US) {
        if (++overload_run >= OVERLOAD_BLOCKS) {
            stop_voices();
            overload_hold = OVERLOAD_HOLD;
            engine_status.overloads++;
            overload_run = 0;
        }
    } else {
        overload_run = 0;
    }
}
