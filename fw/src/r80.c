#include "r80.h"

#include <arm_neon.h>
#include <string.h>

/* Number formats (spec section 1), and where this departs from them:
 * - Tank memory is 18-bit as the unit's, in 32-bit words: three rings of
 *   65 536 words, each followed by a copy of its first GUARD words so a
 *   block's run of reads never wraps (tap indices are set once a block).
 *   Rounded, a loop idles in a limit cycle near -90 dBFS.
 * - Tank arithmetic is integer: Q17 samples times the 1/256 coefficients,
 *   rounded back to Q17 at each nested stage (the unit keeps a whole
 *   expression in its accumulator and rounds only what it stores). The
 *   crossovers' one-poles keep five more bits.
 * - The input/output sections run in float, saturating what they store
 *   at +-1 as the unit does; their coefficients follow 6.4's one-pole
 *   once a block, delay lengths glide sample by sample.
 * - The send is mono (both inputs); the output is the wet path only.
 * - The laws run in the foreground (r80_poll); the control step's
 *   modulators run here, every 960 frames. */

#define RING            65536u
#define GUARD           32u             /* >= BLOCK_MAX - 1 */
#define STRIDE          (RING + 2592u)  /* staggers the rings across cache sets */
#define CONTROL_FRAMES  960u
#define FULL_SCALE      131072.0f       /* Q17: the tank's 18 bits */
#define WET_SCALE       1073741824.0f   /* Q30 */
#define SEND_SCALE      (1.0f / 2147483648.0f)
#define CLEAR_BYTES     32768u
#define FADE_FRAMES     384u            /* an offset change: out, switch, in */
#define XFADE_FRAMES    171u            /* a delay jump (4.13) */
#define BLOCK_MAX       32u
#define SMOOTH_32       0.032780f       /* 1 - exp(-32/960): 6.4's one-pole over a block */
#define DIFF_LEN        2048u

static int32_t mem[3 * STRIDE] __attribute__((aligned(64)));
static int32_t *const m0 = mem, *const m1 = mem + STRIDE, *const m2 = mem + 2 * STRIDE;
static float post_line[RING][2], voice_line[RING][2], diff_line[4][DIFF_LEN];
static int32_t send_q[BLOCK_MAX][2], ret_q[BLOCK_MAX][2];
static float in_x[BLOCK_MAX];
static const uint16_t diff_delay[4] = { 741, 1995, 1083, 1653 };

extern const uint8_t r80_sine[128], r80_levelmap[128], r80_sweep[11];

/* ---- foreground <-> audio ---- */

static struct {
    const struct r80_program *load;     /* with tgt[load_tgt] */
    volatile uint32_t load_seq, tgt_seq;
    volatile uint8_t load_tgt, tgt_idx;
    struct r80_target tgt[2];
    int16_t src[2][R80_SOURCES];        /* the modulators' outputs, -1 no value */
    volatile uint32_t src_seq;
    volatile uint8_t src_idx;
} hand;

/* ---- audio state ---- */

struct dline { float len[2], dlen[2], old[2]; uint16_t xf[2]; };

static struct {
    const struct r80_program *p;
    uint32_t load_seen, tgt_seen, clearing, since;
    uint8_t algo, ready;
    struct r80_target t;
    /* tank: i the next sample's position, ix[s] step s's position at the
     * block's first sample (i0) */
    uint32_t i, i0, ix[R80_K];
    int16_t k[R80_K], kret[12];
    uint16_t a[R80_K];
    int32_t s[8], sl, sr;
    /* coefficient ramps (6.4): 16-bit units */
    int32_t k16[R80_K], kt16[R80_K], krate[R80_K];
    uint8_t coarse[R80_K], owned[R80_K], aowned[R80_K];
    uint32_t ramping[4], ramp_acc, ramp_q8;
    /* offsets: fade out, switch, fade in */
    uint16_t a_next[R80_K];
    uint8_t fading, fade_pending, restart;
    uint16_t geometry;
    int32_t rg;
    uint32_t rclear;
    int32_t fade_g, fade_step;
    /* AP Sweep (6.1) */
    uint8_t npairs, pa[10], pb[10], halved[10], cb[10], crest[10], visit, phase, visiting, tstep;
    int8_t sa[10], sb[10];
    uint32_t sweep_phase, sweep_rate, sweep_tick;
    /* Concert Hall taps (6.2) */
    uint8_t code[4];
    int8_t dir[4], step[4];
    int32_t countdown;
    uint32_t next, tap_phase, tap_rate;
    /* input/output section */
    float io[R80_IO_MAX];
    struct dline post, v13, v24;
    float post_r32, voice_r32;
    float ret[2], fb[2], lp[2];
    uint8_t gate[6];
    /* Chorus+Rvb voices (3.1) */
    float la[2], cv[8], cm[8];
    int64_t cb_q[6];                    /* b_j, 1/128 sample x 2^16 */
    int32_t ca_q[6];                    /* a_j, 1/128 sample x 2^11 */
    int32_t phi[8], rate[8];
    uint32_t u, ncount, kvoice, ksmooth;
    uint8_t cglide;
    /* control step */
    uint32_t ctl;
    float peak;
} st;

/* Calls may be shorter than BLOCK_MAX: what runs once a block (edits,
 * ramps, smoothing, glides, clears, restarts) runs once per SPAN frames, at
 * its first and last call, as with 32-frame calls. */
#define SPAN            32u
static unsigned span_f;                 /* frames of the span before this call */
static uint8_t span_first, span_last, span_mute;

static uint8_t fade_class[R80_K];
static int32_t fade_gain[2] = { 256, 256 };

/* ---- fixed point ---- */

#ifdef __ARM_FEATURE_SAT
#define SAT(v, bits) __builtin_arm_ssat((v), (bits))
#else
#define SAT(v, bits) ((v) > (1 << ((bits) - 1)) - 1 ? (1 << ((bits) - 1)) - 1 : (v) < -(1 << ((bits) - 1)) ? -(1 << ((bits) - 1)) : (v))
#endif

static inline int32_t sat18(int32_t v) { return SAT(v, 18); }

/* Q25 (Q17 x 1/256 steps) back to Q17. */
static inline int32_t q17(int32_t v) { return (v + 128) >> 8; }

/* A crossover's one-pole in Q22: its state coefficient is 251/256 or so,
 * and rounded to Q17 it would stick up to 25 steps off zero. */
static inline int32_t lp_step(int32_t y, int32_t x, int32_t ky, int32_t kx)
{
    y = (y * ky + x * kx * 32 + 128) >> 8;
    return SAT(y, 23);
}

static inline void put(int32_t *m, uint32_t idx, int32_t v)
{
    idx &= RING - 1u;
    m[idx] = v;
    if (idx < GUARD) {
        m[idx + RING] = v;
    }
}

static inline void reindex(unsigned s)
{
    st.ix[s] = (st.i0 - st.a[s]) & (RING - 1u);
}

/* ---- AP Sweep (6.1) ---- */

static void sweep_timing(void)
{
    int x = st.t.apsweep * 32768 / 50, b = 255 - 178 * x / 32768, q;

    b = b < 1 ? 1 : b;
    q = b * r80_sweep[st.tstep] / 128;
    q = q >= 255 ? 255 : q + 1;
    /* A step every 0.130 P samples. */
    st.sweep_rate = (uint32_t)(65536.0f / (0.130f * (float)q) + 0.5f);
}

static void pair_set(unsigned j)
{
    int g = 255 - st.cb[j];

    st.k[st.pb[j]] = (int16_t)(st.sb[j] * g);
    st.k[st.pa[j]] = (int16_t)(st.sa[j] * (st.halved[j] ? r80_ap_first(g) : g));
}

static void sweep_step(void)
{
    int busy = -1;

    if (st.visiting) {
        unsigned j = st.visit;
        busy = (int)j;
        if (!st.phase) {
            if (st.cb[j] < 255) {
                st.cb[j]++;
            }
            if (st.cb[j] == 255) {
                st.sa[j] = (int8_t)-st.sa[j];
                st.sb[j] = (int8_t)-st.sb[j];
                st.phase = 1;
            }
        } else if (st.cb[j] != st.crest[j]) {
            st.cb[j] = (uint8_t)(st.cb[j] + (st.cb[j] > st.crest[j] ? -1 : 1));
        } else {
            st.visiting = 0;
            st.visit = (uint8_t)((j + 1u) % st.npairs);
        }
        pair_set(j);
    } else if (st.t.apsweep) {
        st.visiting = 1;
        st.phase = 0;
    }
    for (unsigned j = 0; j < st.npairs; j++) {
        if ((int)j != busy && st.cb[j] != st.crest[j]) {
            st.cb[j] = (uint8_t)(st.cb[j] + (st.cb[j] > st.crest[j] ? -1 : 1));
            pair_set(j);
        }
    }
}

static int sweep_busy(void)
{
    if (st.t.apsweep || st.visiting) {
        return 1;
    }
    for (unsigned j = 0; j < st.npairs; j++) {
        if (st.cb[j] != st.crest[j]) {
            return 1;
        }
    }
    return 0;
}

/* ---- Concert Hall modulated taps (6.2) ---- */

static const uint8_t tap_a[4] = { 109, 117, 123, 3 };
static const uint8_t tap_b[4] = { 112, 118, 124, 4 };

static void tap_step(void)
{
    if (--st.countdown >= 0) {
        for (unsigned j = 0; j < 4; j++) {
            st.code[j] = (uint8_t)(st.code[j] + st.dir[j]);
        }
        return;
    }
    unsigned j = st.next;

    st.next = (st.next + 1u) & 3u;
    st.countdown = st.next ? 64 : 63;
    st.dir[j] = (int8_t)-st.dir[j];
    int s = st.step[j];
    /* At the fraction's end the read that moves carries no weight. */
    if (st.code[j] == 255u) {
        uint16_t *b = &st.a[tap_b[j]];
        unsigned x = *b & 255u;
        if (s < 0 && ((x + (unsigned)s) & 255u) == 255u) {
            st.step[j] = (int8_t)-s;
        } else {
            *b = (uint16_t)((*b & ~255u) | ((x + (unsigned)s) & 255u));
            reindex(tap_b[j]);
        }
    } else {
        uint16_t *a = &st.a[tap_a[j]];
        unsigned x = *a & 255u;
        if (s > 0 && ((x + (unsigned)s) & 255u) == 0u) {
            st.step[j] = (int8_t)-s;
        } else {
            *a = (uint16_t)((*a & ~255u) | ((x + (unsigned)s) & 255u));
            reindex(tap_a[j]);
        }
    }
}

/* The sample-timed processes, before each tank sample. */
static inline void events(void)
{
    if (st.sweep_rate) {
        st.sweep_phase += st.sweep_rate;
        if (st.sweep_phase >= 65536u) {
            st.sweep_phase -= 65536u;
            sweep_step();
        }
    }
    if (st.tap_rate) {
        st.tap_phase += st.tap_rate;
        if (st.tap_phase >= 65536u) {
            st.tap_phase -= 65536u;
            tap_step();
        }
    }
}

static inline void fade_tick(void)
{
    st.fade_g += st.fade_step;
    if (st.fade_g <= 0) {
        st.fade_g = 0;
        st.fade_step = -st.fade_step;
        for (unsigned s = 0; s < R80_K; s++) {
            if (fade_class[s]) {
                st.a[s] = st.a_next[s];
                reindex(s);
            }
        }
    } else if (st.fade_g >= 256 << 16) {
        st.fade_g = 256 << 16;
        st.fade_step = 0;
    }
    fade_gain[1] = st.fade_g >> 16;
}

#include "r80_steps.h"

/* Each tank's memory steps (ring << 8 | step) and fixed offsets (ring << 16 | offset). */
struct steps { const uint16_t *s; const uint32_t *fixed; uint8_t n, nfixed; };
#define STEPS(name) { steps_##name, fixed_##name, sizeof(steps_##name) / 2u, sizeof(fixed_##name) / 4u }
static const struct steps tank_steps[4] = { STEPS(chamber), STEPS(plate), STEPS(hall), STEPS(inverse) };

/* A block's run of one tap (32 words). */
static inline void pld_run(const int32_t *p)
{
    __builtin_prefetch(p);
    __builtin_prefetch(p + 16);
    __builtin_prefetch(p + 31);
}

/* The block's tap positions; their runs prefetched. */
static void tank_index(void)
{
    const struct steps *t = &tank_steps[st.algo == R80_CHORUS ? R80_PLATE : st.algo];

    st.i0 = st.i;
    for (unsigned j = 0; j < t->n; j++) {
        unsigned s = t->s[j] & 255u;
        reindex(s);
        pld_run(mem + (t->s[j] >> 8) * STRIDE + st.ix[s]);
    }
    for (unsigned j = 0; j < t->nfixed; j++) {
        pld_run(mem + (t->fixed[j] >> 16) * STRIDE + ((st.i0 - t->fixed[j]) & (RING - 1u)));
    }
}

/* The next block's runs, prefetched while the rest of the block runs. */
static void tank_prefetch(unsigned count)
{
    const struct steps *t = &tank_steps[st.algo == R80_CHORUS ? R80_PLATE : st.algo];

    for (unsigned j = 0; j < t->n; j++) {
        pld_run(mem + (t->s[j] >> 8) * STRIDE + ((st.ix[t->s[j] & 255u] + count) & (RING - 1u)));
    }
    for (unsigned j = 0; j < t->nfixed; j++) {
        pld_run(mem + (t->fixed[j] >> 16) * STRIDE + ((st.i - t->fixed[j]) & (RING - 1u)));
    }
}

#define TANK_FADE 0
#define TANK(name) name##_plain
#include "r80_tanks.h"
#undef TANK
#undef TANK_FADE
#define TANK_FADE 1
#define TANK(name) name##_faded
#include "r80_tanks.h"
#undef TANK
#undef TANK_FADE

/* ---- tank coefficients ---- */

/* Coefficients kept in sixteenths (6.4): Shape/Attack, Rvb Out, RefLvl, Depth. */
static const uint8_t coarse_chamber[] = { 13, 47, 24, 113, 14, 48, 25, 114, 68, 86, 70, 88, 69, 87 };
static const uint8_t coarse_plate[] = { 10, 43, 28, 122, 2, 35, 61, 78, 63, 80, 62, 79 };
static const uint8_t coarse_hall[] = { 13, 14, 15, 16, 18, 19, 20, 21, 23, 25, 24, 26 };
static const uint8_t coarse_inverse[] = { 11, 12, 13, 14, 45, 46, 47, 48, 70, 87, 69, 86 };

static const uint8_t pairs_plate[6][2] = { { 123, 125 }, { 3, 5 }, { 11, 13 }, { 29, 31 }, { 36, 38 }, { 44, 46 } };
static const uint8_t pairs_chamber[6][2] = { { 0, 2 }, { 6, 8 }, { 15, 17 }, { 34, 36 }, { 40, 42 }, { 49, 51 } };
static const uint8_t pairs_hall[10][2] = { { 46, 52 }, { 48, 50 }, { 54, 64 }, { 56, 62 }, { 58, 60 },
                                           { 91, 97 }, { 93, 95 }, { 99, 109 }, { 101, 107 }, { 103, 105 } };
static const uint8_t halved_hall[10] = { 0, 1, 0, 0, 1, 0, 1, 0, 0, 1 };

static void derive(void)
{
    if (st.algo != R80_HALL) {
        return;
    }
    const int16_t *k = st.k;
    /* retL = k25 (k18 X18 + ... + k22 X22) + k26 X26, retR likewise: whole
     * 1/256 steps (sixteenths times sixteenths). */
    st.kret[0] = (int16_t)(k[18] * k[25] / 256);
    st.kret[1] = (int16_t)(k[19] * k[25] / 256);
    st.kret[2] = (int16_t)(k[20] * k[25] / 256);
    st.kret[3] = (int16_t)(k[21] * k[25] / 256);
    st.kret[4] = (int16_t)(k[22] * k[25] / 256);
    st.kret[5] = k[26];
    st.kret[6] = (int16_t)(k[14] * k[23] / 256);
    st.kret[7] = (int16_t)(k[13] * k[23] / 256);
    st.kret[8] = (int16_t)(k[15] * k[23] / 256);
    st.kret[9] = (int16_t)(k[16] * k[23] / 256);
    st.kret[10] = (int16_t)(k[17] * k[23] / 256);
    st.kret[11] = k[24];
}

static void ramps_block(void)
{
    if (!(st.ramping[0] | st.ramping[1] | st.ramping[2] | st.ramping[3])) {
        return;
    }
    st.ramp_acc += st.ramp_q8;
    int32_t n = (int32_t)(st.ramp_acc >> 8);
    st.ramp_acc &= 255u;
    if (!n) {
        return;
    }
    for (unsigned w = 0; w < 4; w++) {
        for (uint32_t m = st.ramping[w]; m; m &= m - 1u) {
            unsigned s = 32u * w + (unsigned)__builtin_ctz(m);
            int32_t d = st.kt16[s] - st.k16[s], move = st.krate[s] * n;
            if (d > move) {
                st.k16[s] += move;
            } else if (-d > move) {
                st.k16[s] -= move;
            } else {
                st.k16[s] = st.kt16[s];
                st.ramping[w] &= ~(1u << (s & 31u));
            }
            st.k[s] = (int16_t)(st.coarse[s] ? st.k16[s] / 2048 * 16 : st.k16[s] / 128);
        }
    }
    derive();
}

/* ---- the target ---- */

/* A new delay length: within the glide range it glides (dline_block),
 * past it the tap jumps under a cross-fade (4.13). */
static void delay_target(struct dline *d, unsigned lane, float target, unsigned range, int jump)
{
    float now = d->len[lane];

    if (jump) {
        d->len[lane] = target;
        d->dlen[lane] = 0.0f;
        d->xf[lane] = 0;
        return;
    }
    if (target == now) {
        return;
    }
    float diff = target > now ? target - now : now - target;
    if (diff <= (float)range) {
        return;
    }
    d->old[lane] = now;
    d->len[lane] = target;
    d->dlen[lane] = 0.0f;
    d->xf[lane] = XFADE_FRAMES;
}

static float target_len(unsigned j)
{
    return (float)st.t.io[j];
}

/* 1 - (1 - G/2^23)^32: a block of 4.13's per-sample glide. */
static float glide32(unsigned g)
{
    float k = 1.0f - (float)g / 8388608.0f, p = k;

    for (unsigned b = 0; b < 5; b++) {
        p *= p;
    }
    (void)k;
    return 1.0f - p;
}

static void apply_target(const struct r80_target *t, int load)
{
    st.t = *t;
    for (unsigned s = 0; s < R80_K; s++) {
        if (st.owned[s]) {
            continue;
        }
        if (load) {
            st.k[s] = t->k[s];
            st.k16[s] = st.kt16[s] = t->k[s] * 128;
            continue;
        }
        if (t->k[s] * 128 != st.kt16[s]) {
            int32_t d;
            st.kt16[s] = t->k[s] * 128;
            d = st.kt16[s] - st.k16[s];
            d = d < 0 ? -d : d;
            st.krate[s] = 2 * ((d / 128) | 1);
            st.ramping[s >> 5] |= 1u << (s & 31u);
        }
    }
    for (unsigned j = 0; j < st.npairs; j++) {
        st.crest[j] = (uint8_t)(255 - (t->k[st.pb[j]] < 0 ? -t->k[st.pb[j]] : t->k[st.pb[j]]));
    }
    for (unsigned s = 0; s < R80_K; s++) {
        int tap = st.algo == R80_HALL && (s == 3 || s == 4 || s == 109 || s == 112 || s == 117 || s == 118 ||
                                          s == 123 || s == 124);
        if (load) {
            st.a[s] = st.a_next[s] = t->a[s];
        } else if (!tap && t->a[s] != st.a_next[s]) {
            st.a_next[s] = t->a[s];
            st.fade_pending = 1;
        }
    }
    derive();
    sweep_timing();
    st.tap_rate = t->divider ? (uint32_t)(65536.0f / (0.2575f * ((float)t->divider + 4.1f)) + 0.5f) : 0;
    st.post_r32 = glide32(t->post_resp);
    st.voice_r32 = glide32(t->voice_resp);
    if (st.algo != R80_CHORUS) {
        for (unsigned lane = 0; lane < 2; lane++) {
            delay_target(&st.post, lane, target_len(R80_IO_POSTL + lane), t->post_range, load);
            delay_target(&st.v13, lane, target_len(R80_IO_DV1 + 2 * lane), t->voice_range, load);
            delay_target(&st.v24, lane, target_len(R80_IO_DV1 + 2 * lane + 1), t->voice_range, load);
        }
    } else {
        for (unsigned j = 0; j < 6; j++) {
            int64_t b = (int64_t)(t->io[R80_C_B1 + j] * 65536.0);
            if (load) {
                st.cb_q[j] = b;
                st.ca_q[j] = (int32_t)t->io[R80_C_A1 + j] << 11;
            } else if (b != st.cb_q[j]) {
                st.cglide = 1;
            }
        }
    }
}

/* ---- control step: the modulators (6.3, 16.2, 16.5) ---- */

static struct {
    uint32_t lfo, env[2], ar, sw_phi[2];
    int32_t sw_q[2];
    uint8_t ar_state, sw_run[2], sw_restart[2], sw_rising[2], sel, latch_state, latch_has;
    int16_t sw_out[2], latch_x, latch_y;
    int16_t v[R80_SOURCES];
} mod;

static int wave(unsigned i)
{
    unsigned k = i & 255u;
    int w = r80_sine[k < 128u ? k : 255u - k];

    return i < 256u ? w : 127 - w;
}

static int triggered(unsigned which)
{
    unsigned s = st.p->trig_src[which];
    int lvl = st.p->trig_level[which], x = mod.v[s];

    if (s == R80_SRC_NONE) {
        return lvl == 0;
    }
    return x >= 0 && x >= lvl;
}

static void latch(void)
{
    int x = mod.v[st.p->latch_src], lo = st.p->latch_low, hi = st.p->latch_high;

    if (x < 0 || (mod.latch_has && x == mod.latch_x)) {
        return;
    }
    mod.latch_has = 1;
    mod.latch_x = (int16_t)x;
    if (lo >= hi || (lo == 0 && hi == 127)) {
        mod.latch_y = (int16_t)x;
        return;
    }
    /* 0 following; 1-3 latched high (waiting below, above, at or below H);
     * 4-6 latched low, mirrored. */
    switch (mod.latch_state) {
    case 0:
        if (hi < 127 && x >= hi) {
            mod.latch_state = 1;
            mod.latch_y = (int16_t)hi;
        } else if (lo > 0 && x <= lo) {
            mod.latch_state = 4;
            mod.latch_y = (int16_t)lo;
        } else {
            mod.latch_y = (int16_t)x;
        }
        break;
    case 1: mod.latch_state = x < hi ? 2 : 1; break;
    case 2: mod.latch_state = x >= hi ? 3 : 2; break;
    case 3:
        if (x <= hi) {
            mod.latch_state = 0;
            mod.latch_y = (int16_t)x;
        }
        break;
    case 4: mod.latch_state = x > lo ? 5 : 4; break;
    case 5: mod.latch_state = x <= lo ? 6 : 5; break;
    default:
        if (x >= lo) {
            mod.latch_state = 0;
            mod.latch_y = (int16_t)x;
        }
        break;
    }
}

static void ar_step(void)
{
    const struct r80_mod *m = &st.t.mod;
    int trig = triggered(0);
    uint32_t e = mod.ar;

    switch (m->ar_mode) {
    case 2:
        if (trig && e < 65535u) {
            e = e + m->ar_a > 65535u ? 65535u : e + m->ar_a;
        } else if (!trig && e) {
            e = e > m->ar_r ? e - m->ar_r : 0;
        }
        break;
    case 1:
    case 3:
        /* 0 rest, 1 attack, 2 top (One Shot holds), 3 release. */
        if (mod.ar_state == 0 && trig) {
            mod.ar_state = 1;
        }
        if (mod.ar_state == 1) {
            e = e + m->ar_a >= 65535u ? 65535u : e + m->ar_a;
            if (e == 65535u) {
                mod.ar_state = m->ar_mode == 3 ? 3 : 2;
            }
        } else if (mod.ar_state == 2) {
            if (!trig) {
                mod.ar_state = 3;
            }
        } else if (mod.ar_state == 3) {
            e = e > m->ar_r ? e - m->ar_r : 0;
            if (!e) {
                mod.ar_state = m->ar_mode == 3 && trig ? 1 : 0;
            }
        }
        break;
    default:
        e = 0;
        break;
    }
    mod.ar = e;
}

/* Returns 1 when its cycle ended in this step. */
static int sw_step(unsigned j)
{
    const struct r80_mod *m = &st.t.mod;
    int trig = triggered(1 + j), ended = 0;

    if (!m->sw_mode[j]) {
        mod.sw_out[j] = -1;
        return 0;
    }
    if (mod.sw_out[j] < 0) {
        mod.sw_out[j] = 0;
    }
    if (trig) {
        mod.sw_run[j] = 1;
    }
    if (!mod.sw_run[j]) {
        mod.sw_restart[j] = 1;
        return 0;
    }
    unsigned i = 0;
    if (mod.sw_restart[j]) {
        mod.sw_phi[j] = 0;
        mod.sw_q[j] = 0;
        mod.sw_restart[j] = 0;
        mod.sw_rising[j] = 1;
    } else {
        i = (mod.sw_phi[j] >> 8) & 0x1ffu;
    }
    int out;
    if (m->sw_mode[j] == 1) {
        out = i < m->sw_pw[j] ? 127 : 0;
    } else {
        out = mod.sw_q[j] >> 8;
        if (i < m->sw_pw[j]) {
            if (!mod.sw_rising[j]) {
                mod.sw_rising[j] = 1;
                mod.sw_q[j] = 0;
            }
            mod.sw_q[j] = mod.sw_q[j] + m->sw_up[j] > 0x7f00 ? 0x7f00 : mod.sw_q[j] + m->sw_up[j];
        } else {
            if (mod.sw_rising[j]) {
                mod.sw_rising[j] = 0;
                mod.sw_q[j] = 0x7f00;
            }
            mod.sw_q[j] -= m->sw_down[j];
            mod.sw_q[j] = mod.sw_q[j] < 0 ? 0 : mod.sw_q[j];
        }
    }
    mod.sw_phi[j] += m->sw_inc[j];
    if (mod.sw_phi[j] >= 1u << 17) {
        mod.sw_phi[j] -= 1u << 17;
        mod.sw_run[j] = 0;
        ended = 1;
        if (!trig) {
            out = 0;
        }
    }
    mod.sw_out[j] = (int16_t)out;
    return ended;
}

static void control_step(void)
{
    const struct r80_mod *m = &st.t.mod;
    int16_t *v = mod.v;
    int p = (int)(st.peak * 128.0f);

    st.peak = 0.0f;
    p = p > 127 ? 127 : p;
    v[R80_SRC_LEFT_LVL] = v[R80_SRC_RIGHT_LVL] = v[R80_SRC_MONO_LVL] = r80_levelmap[p];
    latch();
    v[R80_SRC_LATCH] = mod.latch_has ? mod.latch_y : -1;
    ar_step();
    v[R80_SRC_AR] = (int16_t)(mod.ar >> 9);
    int end0 = sw_step(0), end1 = sw_step(1);
    v[R80_SRC_SW1] = mod.sw_out[0];
    v[R80_SRC_SW2] = mod.sw_out[1];
    v[R80_SRC_SW12] = mod.sw_out[mod.sel];
    if ((mod.sel == 0 && end0) || (mod.sel == 1 && end1)) {
        mod.sel ^= 1u;
    }
    unsigned i = ((mod.lfo + 128u) >> 8) & 511u;
    int w[6] = { wave(i), wave((i + 128u) & 511u), i < 256u ? 127 : 0, (int)(i >> 2), i < m->lfo_pw ? 127 : 0,
                 i < 256u ? (int)(i >> 1) : 255 - (int)(i >> 1) };
    v[R80_SRC_SINE] = (int16_t)w[0];
    v[R80_SRC_COSINE] = (int16_t)w[1];
    v[R80_SRC_SQUARE] = (int16_t)w[2];
    v[R80_SRC_SAWTOOTH] = (int16_t)w[3];
    v[R80_SRC_PULSE] = (int16_t)w[4];
    v[R80_SRC_TRIANGLE] = (int16_t)w[5];
    v[R80_SRC_LFO] = (int16_t)(w[m->lfo_shape < 6 ? m->lfo_shape : 0] * m->lfo_dm / 256);
    mod.lfo += m->lfo_inc;
    for (unsigned s = 0; s < 2; s++) {
        uint32_t t = 516u * (uint32_t)v[s ? R80_SRC_RIGHT_LVL : R80_SRC_LEFT_LVL], e = mod.env[s];
        if (e <= t || e - t < m->env_r[s]) {
            e = t;
        } else {
            e -= m->env_r[s];
        }
        mod.env[s] = e;
        v[s ? R80_SRC_ENV_R : R80_SRC_ENV_L] = (int16_t)(e >> 9);
    }
    uint8_t w2 = (uint8_t)(hand.src_idx ^ 1u);
    memcpy(hand.src[w2], v, sizeof(mod.v));
    __asm__ volatile("" ::: "memory");
    hand.src_idx = w2;
    hand.src_seq++;
}

static void mod_reset(void)
{
    memset(&mod, 0, sizeof(mod));
    for (unsigned s = 0; s < R80_SOURCES; s++) {
        mod.v[s] = 0;
    }
    mod.v[R80_SRC_NONE] = mod.v[R80_SRC_NOVALUE] = mod.v[R80_SRC_ADJUST] = -1;
    mod.v[R80_SRC_ON] = 127;
    mod.sw_restart[0] = mod.sw_restart[1] = 1;
}

/* ---- load ---- */

static void load(const struct r80_program *p, const struct r80_target *t)
{
    memset(&st.s, 0, sizeof(st.s));
    st.p = p;
    st.algo = p ? p->algo : 0;
    st.ready = 0;
    st.since = 0;
    st.sl = st.sr = 0;
    st.i = 0;
    st.ctl = 0;
    st.peak = 0.0f;
    st.fading = st.fade_pending = st.restart = 0;
    st.geometry = t->geometry;
    st.fade_g = 256 << 16;
    st.fade_step = 0;
    fade_gain[1] = 256;
    memset(st.ramping, 0, sizeof(st.ramping));
    st.ramp_acc = 0;
    st.ramp_q8 = st.algo == R80_HALL ? 152u : 3277u;    /* ramp steps every 54 / 2.5 samples */
    if (!p) {
        return;
    }
    memset(st.coarse, 0, sizeof(st.coarse));
    memset(st.owned, 0, sizeof(st.owned));
    memset(st.aowned, 0, sizeof(st.aowned));
    const uint8_t *c = st.algo == R80_CHAMBER ? coarse_chamber : st.algo == R80_HALL ? coarse_hall :
                       st.algo == R80_INVERSE ? coarse_inverse : coarse_plate;
    unsigned nc = st.algo == R80_CHAMBER ? sizeof(coarse_chamber) : st.algo == R80_HALL ? sizeof(coarse_hall) :
                  st.algo == R80_INVERSE ? sizeof(coarse_inverse) : sizeof(coarse_plate);
    for (unsigned j = 0; j < nc; j++) {
        st.coarse[c[j]] = 1;
    }
    st.npairs = st.algo == R80_HALL ? 10 : st.algo == R80_INVERSE ? 0 : 6;
    for (unsigned j = 0; j < st.npairs; j++) {
        const uint8_t *pr = st.algo == R80_HALL ? pairs_hall[j] : st.algo == R80_CHAMBER ? pairs_chamber[j] :
                            pairs_plate[j];
        st.pa[j] = pr[0];
        st.pb[j] = pr[1];
        st.halved[j] = st.algo == R80_HALL ? halved_hall[j] : 1;
        st.sa[j] = (int8_t)(t->k[pr[0]] < 0 ? -1 : 1);
        st.sb[j] = (int8_t)(t->k[pr[1]] < 0 ? -1 : 1);
        st.owned[pr[0]] = st.owned[pr[1]] = 1;
    }
    if (st.algo == R80_HALL) {
        static const uint8_t code0[4] = { 0xC0, 0x7F, 0x40, 0xFF };
        st.owned[1] = st.owned[7] = st.owned[115] = st.owned[121] = 1;
        for (unsigned j = 0; j < 4; j++) {
            st.aowned[tap_a[j]] = st.aowned[tap_b[j]] = 1;
            st.code[j] = code0[j];
            st.dir[j] = (int8_t)(j & 1 ? -1 : 1);
            st.step[j] = (int8_t)(j & 1 ? -2 : 2);
        }
        st.countdown = 63;
        st.next = 0;
        st.tap_phase = 0;
    }
    st.visit = 0;
    st.visiting = 0;
    st.tstep = 0;
    st.sweep_phase = 0;
    st.sweep_tick = 52800;
    apply_target(t, 1);
    for (unsigned j = 0; j < st.npairs; j++) {
        st.cb[j] = st.crest[j];
        pair_set(j);
    }
    memset(st.io, 0, sizeof(st.io));
    st.ret[0] = st.ret[1] = st.fb[0] = st.fb[1] = st.lp[0] = st.lp[1] = 0.0f;
    st.la[0] = st.la[1] = 0.0f;
    for (unsigned j = 0; j < 6; j++) {
        st.gate[j] = 0;
        st.cv[j] = st.cm[j] = 0.0f;
        st.phi[j] = 0;
        st.rate[j] = (int32_t)t->io[R80_C_R1 + j];
    }
    st.u = (uint32_t)t->io[R80_C_U0];
    st.ncount = (uint32_t)t->io[R80_C_N];
    st.kvoice = st.ksmooth = 0;
    st.cglide = 0;
    mod_reset();
    st.clearing = 1;
}

static int clear_step(void)
{
    static uint8_t *const regions[] = { (uint8_t *)mem, (uint8_t *)post_line, (uint8_t *)voice_line,
                                        (uint8_t *)diff_line };
    static const uint32_t sizes[] = { sizeof(mem), sizeof(post_line), sizeof(voice_line), sizeof(diff_line) };
    uint32_t at = st.clearing - 1u;

    for (unsigned r = 0; r < sizeof(sizes) / sizeof(sizes[0]); r++) {
        if (at < sizes[r]) {
            uint32_t len = sizes[r] - at < CLEAR_BYTES ? sizes[r] - at : CLEAR_BYTES;
            float32x4_t z = vdupq_n_f32(0.0f);
            for (float *q = (float *)(regions[r] + at), *e = q + len / 4u; q < e; q += 4) {
                vst1q_f32(q, z);
            }
            st.clearing += len;
            return 1;
        }
        at -= sizes[r];
    }
    st.clearing = 0;
    return 0;
}

/* The tank over a block. A Size / Duration change moves the loop's own
 * writes, and old and new data would meet with a jump wherever a read
 * crosses them later: the tank's send and return fade out, its memory
 * clears, the tank restarts at the new size and its send fades in (the
 * voices and post-delays run on). Other offset changes fade their steps
 * (6.4). */
static void tank_run(unsigned count)
{
    if (st.restart == 2) {
        memset(ret_q, 0, sizeof(ret_q[0]) * count);
        uint32_t at = st.rclear, len = sizeof(mem) - at < 2u * CLEAR_BYTES ? sizeof(mem) - at : 2u * CLEAR_BYTES;
        len = len * count / SPAN;
        float32x4_t z = vdupq_n_f32(0.0f);
        for (float *q = (float *)((uint8_t *)mem + at), *e = q + len / 4u; q < e; q += 4) {
            vst1q_f32(q, z);
        }
        st.rclear += len;
        st.i = (st.i + count) & (RING - 1u);
        if (span_last && st.rclear >= sizeof(mem)) {
            for (unsigned s = 0; s < R80_K; s++) {
                if (!st.aowned[s]) {
                    st.a[s] = st.a_next[s];
                }
            }
            memset(st.s, 0, sizeof(st.s));
            st.sl = st.sr = 0;
            st.restart = 3;
        }
        return;
    }
    int faded = st.fade_step || st.fading;
    int32_t gain[BLOCK_MAX];
    if (st.restart) {
        int32_t step = (int32_t)((256u << 16) / FADE_FRAMES) * (st.restart == 1 ? -1 : 1);
        for (unsigned f = 0; f < count; f++) {
            gain[f] = st.rg >> 16;
            send_q[f][0] = send_q[f][0] * gain[f] >> 8;
            send_q[f][1] = send_q[f][1] * gain[f] >> 8;
            st.rg += step;
            st.rg = st.rg < 0 ? 0 : st.rg > 256 << 16 ? 256 << 16 : st.rg;
        }
    }
    tank_index();
    switch (st.algo) {
    case R80_CHAMBER: faded ? chamber_faded(count) : chamber_plain(count); break;
    case R80_HALL: faded ? hall_faded(count) : hall_plain(count); break;
    case R80_INVERSE: faded ? inverse_faded(count) : inverse_plain(count); break;
    default: faded ? plate_faded(count) : plate_plain(count); break;
    }
    tank_prefetch(count);
    if (st.restart) {
        if (st.restart == 1) {
            for (unsigned f = 0; f < count; f++) {
                ret_q[f][0] = ret_q[f][0] * gain[f] >> 8;
                ret_q[f][1] = ret_q[f][1] * gain[f] >> 8;
            }
        }
        if (span_last && st.restart == 1 && !st.rg) {
            st.restart = 2;
            st.rclear = 0;
        } else if (span_last && st.restart == 3 && st.rg == 256 << 16) {
            st.restart = 0;
        }
    }
}

/* ---- input/output section (3) ---- */

static inline float32x2_t clip1(float32x2_t v)
{
    return vmax_f32(vmin_f32(v, vdup_n_f32(1.0f)), vdup_n_f32(-1.0f));
}

static inline float clipf(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

/* Two lanes of a line pair, each at its own (fractional) delay. */
static inline float32x2_t line_read(float (*line)[2], uint32_t n, float32x2_t len)
{
    uint32x2_t i = vcvt_u32_f32(len);
    float32x2_t f = vsub_f32(len, vcvt_f32_u32(i)), x = vdup_n_f32(0.0f), y = x;
    uint32_t i0 = vget_lane_u32(i, 0), i1 = vget_lane_u32(i, 1);

    x = vld1_lane_f32(&line[(uint16_t)(n - i0)][0], x, 0);
    x = vld1_lane_f32(&line[(uint16_t)(n - i1)][1], x, 1);
    y = vld1_lane_f32(&line[(uint16_t)(n - i0 - 1u)][0], y, 0);
    y = vld1_lane_f32(&line[(uint16_t)(n - i1 - 1u)][1], y, 1);
    return vmla_f32(x, vsub_f32(y, x), f);
}

static inline float32x2_t dline_read(float (*line)[2], uint32_t n, struct dline *d)
{
    float32x2_t len = vld1_f32(d->len), y = line_read(line, n, len);

    vst1_f32(d->len, vadd_f32(len, vld1_f32(d->dlen)));
    if (d->xf[0] | d->xf[1]) {
        float32x2_t o = line_read(line, n, vld1_f32(d->old));
        float w[2] = { d->xf[0] / (float)XFADE_FRAMES, d->xf[1] / (float)XFADE_FRAMES };
        y = vmla_f32(y, vsub_f32(o, y), vld1_f32(w));
        d->xf[0] = (uint16_t)(d->xf[0] ? d->xf[0] - 1 : 0);
        d->xf[1] = (uint16_t)(d->xf[1] ? d->xf[1] - 1 : 0);
    }
    return y;
}

/* A block's run of a line tap that reads rows n - d - 1 .. */
static inline void pld_line(float (*line)[2], uint32_t n, uint32_t d)
{
    uint32_t row = n - d - 1u;

    for (unsigned r = 0; r <= BLOCK_MAX; r += 8u) {
        __builtin_prefetch(line[(uint16_t)(row + r)]);
    }
}

/* A delay's glide over the next block (4.13), linear within it. */
static void dline_block(struct dline *d, const float *target, float r32, unsigned count)
{
    for (unsigned l = 0; l < 2; l++) {
        float now = d->len[l], t = target[l];
        if (d->xf[l] || now == t) {
            d->dlen[l] = 0.0f;
            continue;
        }
        float end = t + (now - t) * (1.0f - r32);
        if ((end - t) * (end - t) < 1e-6f) {
            end = t;
        }
        d->dlen[l] = (end - now) / (float)count;
    }
}

static void smooth(unsigned from, unsigned to)
{
    for (unsigned j = from; j < to; j++) {
        float t = (float)st.t.io[j], d = t - st.io[j];
        st.io[j] = d * d < 1e-14f ? t : st.io[j] + SMOOTH_32 * d;
    }
}

static void gates(unsigned voices, const float *delay)
{
    for (unsigned j = 0; j < voices; j++) {
        st.gate[j] = (st.since >= (uint32_t)delay[j]) && !(st.t.clear && delay[j] > 0.0f);
    }
}

static void section3(int32_t (*wet)[2], unsigned count)
{
    float *io = st.io;
    const float delays[4] = { (float)st.t.io[R80_IO_DV1], (float)st.t.io[R80_IO_DV1 + 1],
                              (float)st.t.io[R80_IO_DV1 + 2], (float)st.t.io[R80_IO_DV1 + 3] };

    if (span_first) {
        gates(4, delays);
        smooth(R80_IO_ALL, R80_IO_POSTL);
        smooth(R80_IO_PD, R80_IO_DV1);
        for (unsigned j = 0; j < 4; j++) {
            if (st.gate[j]) {
                smooth(R80_IO_V1L + 3 * j, R80_IO_V1L + 3 * j + 3);
            }
        }
        smooth(R80_IO_FV, R80_IO3_COUNT);
        const float pt[2] = { (float)st.t.io[R80_IO_POSTL], (float)st.t.io[R80_IO_POSTR] };
        const float v13[2] = { delays[0], delays[2] }, v24[2] = { delays[1], delays[3] };
        dline_block(&st.post, pt, st.post_r32, SPAN);
        dline_block(&st.v13, v13, st.voice_r32, SPAN);
        dline_block(&st.v24, v24, st.voice_r32, SPAN);
    }
    const float mix_in[2] = { io[R80_IO_ALL] + io[R80_IO_ALR], io[R80_IO_ARL] + io[R80_IO_ARR] };
    const float32x2_t msum = vld1_f32(mix_in), gs = vdup_n_f32(io[R80_IO_GS] * FULL_SCALE);
    for (unsigned f = 0; f < count; f++) {
        float32x2_t m = vmul_n_f32(msum, in_x[f]);
        vst1_s32(send_q[f], vcvt_s32_f32(vmul_f32(m, gs)));
    }
    uint32_t n0 = st.i;
    const int voiced = io[R80_IO_FV] != 0.0f, diffused = io[R80_IO_VD] != 0.0f;
    for (unsigned l = 0; l < 2; l++) {
        pld_line(post_line, n0, (uint32_t)st.post.len[l]);
        if (voiced) {
            pld_line(voice_line, n0, (uint32_t)st.v13.len[l]);
            pld_line(voice_line, n0, (uint32_t)st.v24.len[l]);
        }
    }
    tank_run(count);
    const float c[2] = { io[R80_IO_C1], io[R80_IO_C2] }, w[2] = { io[R80_IO_W1], io[R80_IO_W2] };
    const float32x2_t gr = vdup_n_f32(io[R80_IO_GR] / FULL_SCALE), vpd = vdup_n_f32(io[R80_IO_PD]);
    const float32x2_t pr = vdup_n_f32(io[R80_IO_PR]), vc = vld1_f32(c), vw = vld1_f32(w);
    const float32x2_t fv = vdup_n_f32(io[R80_IO_FV]), fr = vdup_n_f32(io[R80_IO_FR]);
    const float32x2_t la = vdup_n_f32(io[R80_IO_A]), og = vdup_n_f32(io[R80_IO_OUT]), wq = vdup_n_f32(WET_SCALE);
    /* The voices' gains as lane pairs (voices 1 and 3, 2 and 4). */
    const float g13l[2] = { io[R80_IO_V1L], io[R80_IO_V1L + 6] }, g24l[2] = { io[R80_IO_V1L + 3], io[R80_IO_V1L + 9] };
    const float g13r[2] = { io[R80_IO_V1L + 1], io[R80_IO_V1L + 7] }, g24r[2] = { io[R80_IO_V1L + 4], io[R80_IO_V1L + 10] };
    const float g13f[2] = { io[R80_IO_V1L + 2], io[R80_IO_V1L + 8] }, g24f[2] = { io[R80_IO_V1L + 5], io[R80_IO_V1L + 11] };
    const float32x2_t vg = vdup_n_f32(io[R80_IO_G]), vdd = vdup_n_f32(io[R80_IO_VD]), vpp = vdup_n_f32(io[R80_IO_VP]);
    float32x2_t ret = vld1_f32(st.ret), lp = vld1_f32(st.lp), fb = vld1_f32(st.fb);

    for (unsigned f = 0; f < count; f++) {
        uint32_t n = n0 + f;
        float32x2_t r = vmul_f32(ret, gr);
        ret = vcvt_f32_s32(vld1_s32(ret_q[f]));
        vst1_f32(post_line[(uint16_t)n], clip1(r));
        float32x2_t t = dline_read(post_line, n, &st.post);
        float32x2_t p = vmla_f32(vmul_f32(t, vpd), r, pr);
        float32x2_t q = vmla_lane_f32(vmul_lane_f32(p, vc, 0), vrev64_f32(p), vc, 1);
        float32x2_t o = vmul_f32(q, fr);
        if (voiced) {
            float32x2_t m = vmul_n_f32(msum, in_x[f]), v = vmul_f32(m, vpp);
            if (diffused) {
                float32x2_t x = m;
                for (unsigned s = 0; s < 2; s++) {
                    float32x2_t d = vdup_n_f32(0.0f);
                    d = vld1_lane_f32(&diff_line[s][(n - diff_delay[s]) & (DIFF_LEN - 1u)], d, 0);
                    d = vld1_lane_f32(&diff_line[2 + s][(n - diff_delay[2 + s]) & (DIFF_LEN - 1u)], d, 1);
                    float32x2_t wv = clip1(vmls_f32(x, vg, d));
                    vst1_lane_f32(&diff_line[s][n & (DIFF_LEN - 1u)], wv, 0);
                    vst1_lane_f32(&diff_line[2 + s][n & (DIFF_LEN - 1u)], wv, 1);
                    x = vmla_f32(d, vg, wv);
                }
                v = vmla_f32(v, x, vdd);
            }
            v = clip1(vadd_f32(v, fb));
            vst1_f32(voice_line[(uint16_t)n], v);
            float32x2_t t13 = dline_read(voice_line, n, &st.v13), t24 = dline_read(voice_line, n, &st.v24);
            float32x2_t bl = vmla_f32(vmul_f32(t13, vld1_f32(g13l)), t24, vld1_f32(g24l));
            float32x2_t br = vmla_f32(vmul_f32(t13, vld1_f32(g13r)), t24, vld1_f32(g24r));
            fb = clip1(vmla_f32(vmul_f32(t13, vld1_f32(g13f)), t24, vld1_f32(g24f)));
            o = vmla_f32(o, vpadd_f32(bl, br), fv);
        }
        lp = clip1(vmla_f32(lp, vsub_f32(o, lp), la));
        float32x2_t u = vmla_lane_f32(vmul_lane_f32(lp, vw, 0), vrev64_f32(lp), vw, 1);
        vst1_s32(wet[f], vcvt_s32_f32(vmul_f32(clip1(vmul_f32(u, og)), wq)));
    }
    vst1_f32(st.ret, ret);
    vst1_f32(st.lp, lp);
    vst1_f32(st.fb, fb);
}

/* ---- Chorus+Rvb input/output section (3.1, with 13.1) ---- */

static inline int32_t sext24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }

static void section31(int32_t (*wet)[2], unsigned count)
{
    float *io = st.io;
    float delays[6];

    for (unsigned j = 0; j < 6; j++) {
        delays[j] = (float)(st.t.io[R80_C_B1 + j] / 128.0 - 1.0);
    }
    if (span_first) {
        gates(6, delays);
        smooth(R80_C_90, R80_C_FB1);
        for (unsigned j = 0; j < 6; j++) {
            if (st.gate[j]) {
                smooth(R80_C_FB1 + j, R80_C_FB1 + j + 1);
                smooth(R80_C_CL1 + j, R80_C_CL1 + j + 1);
                smooth(R80_C_CR1 + j, R80_C_CR1 + j + 1);
            }
        }
        smooth(R80_C_DAMP, R80_C_DAMP + 1);
        smooth(R80_C_AC, R80_IO31_COUNT);
    }
    const float sr_g = io[R80_C_94] * (io[R80_C_90] + io[R80_C_92]), sl_g = io[R80_C_94] * (io[R80_C_91] + io[R80_C_93]);
    for (unsigned f = 0; f < count; f++) {
        send_q[f][0] = (int32_t)(sl_g * in_x[f] * FULL_SCALE);
        send_q[f][1] = (int32_t)(sr_g * in_x[f] * FULL_SCALE);
    }
    uint32_t n0 = st.i;
    for (unsigned j = 0; j < 6; j++) {
        pld_line(voice_line, n0, (uint32_t)((int32_t)st.cm[j] + (int32_t)((st.cb_q[j] + 32768) >> 16) - 128) >> 7);
    }
    tank_run(count);
    const float damp = io[R80_C_DAMP], ac = io[R80_C_AC], ad = io[R80_C_AD] / FULL_SCALE, ae = io[R80_C_AE];
    const float af = io[R80_C_AF], og = io[R80_C_OUT];
    const int64_t g = st.t.voice_resp;
    const uint32_t weyl = (uint32_t)st.t.io[R80_C_U];
    int32_t atgt[6];
    for (unsigned j = 0; j < 6; j++) {
        atgt[j] = (int32_t)st.t.io[R80_C_A1 + j] << 11;
    }
    const float sg[2] = { 2.0f * sl_g, 2.0f * sr_g }, cw[2] = { ae, af };
    const float32x2_t sg2 = vld1_f32(sg), vdamp = vdup_n_f32(damp), vac = vdup_n_f32(ac), vad = vdup_n_f32(ad);
    const float32x2_t vcw = vld1_f32(cw), vog = vdup_n_f32(og), wq = vdup_n_f32(WET_SCALE);
    float32x2_t lab = vld1_f32(st.la), ret = vld1_f32(st.ret);
    /* Six voices in two quads (voices 1-4, 5-6 and two idle lanes). */
    float fba[8] = { 0 }, fbb[8] = { 0 }, cl[8] = { 0 }, cr[8] = { 0 }, caf[8] = { 0 }, x0[8] = { 0 }, x1[8] = { 0 };
    int32_t rb[8] = { 0 };
    for (unsigned j = 0; j < 6; j++) {
        (j < 3 ? fba : fbb)[j] = io[R80_C_FB1 + j];
        cl[j] = io[R80_C_CL1 + j];
        cr[j] = io[R80_C_CR1 + j];
        caf[j] = (float)st.ca_q[j] * (1.0f / 2048.0f);
        rb[j] = (int32_t)((st.cb_q[j] + 32768) >> 16) - 128;
    }
    const float32x4_t fba0 = vld1q_f32(fba), fba1 = vld1q_f32(fba + 4), fbb0 = vld1q_f32(fbb), fbb1 = vld1q_f32(fbb + 4);
    const float32x4_t cl0 = vld1q_f32(cl), cl1 = vld1q_f32(cl + 4), cr0 = vld1q_f32(cr), cr1 = vld1q_f32(cr + 4);
    const float32x4_t half = vdupq_n_f32(0.5f), step = vdupq_n_f32(1.0f / 128.0f), unit = vdupq_n_f32(1.0f / 8388608.0f);
    const int32x4_t low7 = vdupq_n_s32(127);
    float32x4_t v0 = vld1q_f32(st.cv), v1 = vld1q_f32(st.cv + 4), m0v = vld1q_f32(st.cm), m1v = vld1q_f32(st.cm + 4);
    int32x4_t ph0 = vld1q_s32(st.phi), ph1 = vld1q_s32(st.phi + 4), rt0 = vld1q_s32(st.rate), rt1 = vld1q_s32(st.rate + 4);
    int32x4_t rb0 = vld1q_s32(rb), rb1 = vld1q_s32(rb + 4);

    for (unsigned f = 0; f < count; f++) {
        uint32_t n = n0 + f;
        float32x4_t sa = vmlaq_f32(vmulq_f32(v0, fba0), v1, fba1), sb = vmlaq_f32(vmulq_f32(v0, fbb0), v1, fbb1);
        float32x2_t pa = vpadd_f32(vget_low_f32(sa), vget_high_f32(sa)), pb = vpadd_f32(vget_low_f32(sb), vget_high_f32(sb));
        float32x2_t fab = vmla_n_f32(vpadd_f32(pa, pb), sg2, in_x[f]);
        lab = clip1(vmla_f32(lab, vdamp, vsub_f32(fab, lab)));
        vst1_f32(voice_line[(uint16_t)n], lab);
        int32x4_t d0 = vaddq_s32(vcvtq_s32_f32(vaddq_f32(m0v, half)), rb0);
        int32x4_t d1 = vaddq_s32(vcvtq_s32_f32(vaddq_f32(m1v, half)), rb1);
        int32_t di[8];
        vst1q_s32(di, vshrq_n_s32(d0, 7));
        vst1q_s32(di + 4, vshrq_n_s32(d1, 7));
        for (unsigned j = 0; j < 6; j++) {
            uint32_t side = j >= 3, at = n - (uint32_t)di[j];
            x0[j] = voice_line[(uint16_t)at][side];
            x1[j] = voice_line[(uint16_t)(at - 1u)][side];
        }
        float32x4_t fr0 = vmulq_f32(vcvtq_f32_s32(vandq_s32(d0, low7)), step);
        float32x4_t fr1 = vmulq_f32(vcvtq_f32_s32(vandq_s32(d1, low7)), step);
        float32x4_t a0 = vld1q_f32(x0), a1 = vld1q_f32(x0 + 4);
        v0 = vmlaq_f32(a0, vsubq_f32(vld1q_f32(x1), a0), fr0);
        v1 = vmlaq_f32(a1, vsubq_f32(vld1q_f32(x1 + 4), a1), fr1);
        float32x4_t hl = vmlaq_f32(vmulq_f32(v0, cl0), v1, cl1), hr = vmlaq_f32(vmulq_f32(v0, cr0), v1, cr1);
        float32x2_t ch = vpadd_f32(vpadd_f32(vget_low_f32(hl), vget_high_f32(hl)),
                                   vpadd_f32(vget_low_f32(hr), vget_high_f32(hr)));
        float32x2_t o = vmla_f32(vmul_f32(ch, vac), ret, vad);
        ret = vcvt_f32_s32(vld1_s32(ret_q[f]));
        float32x2_t w = vmla_lane_f32(vmul_lane_f32(o, vcw, 0), vrev64_f32(o), vcw, 1);
        vst1_s32(wet[f], vcvt_s32_f32(vmul_f32(clip1(vmul_f32(w, vog)), wq)));
        /* The voices' modulation, for the next sample (3.1's order). */
        if (st.cglide) {
            for (unsigned j = 0; j < 6; j++) {
                int64_t b = (int64_t)(st.t.io[R80_C_B1 + j] * 65536.0);
                st.cb_q[j] += ((b - st.cb_q[j]) * g) >> 23;
                rb[j] = (int32_t)((st.cb_q[j] + 32768) >> 16) - 128;
            }
            rb0 = vld1q_s32(rb);
            rb1 = vld1q_s32(rb + 4);
        }
        m0v = vabsq_f32(vmulq_f32(vmulq_f32(vld1q_f32(caf), unit), vcvtq_f32_s32(ph0)));
        m1v = vabsq_f32(vmulq_f32(vmulq_f32(vld1q_f32(caf + 4), unit), vcvtq_f32_s32(ph1)));
        ph0 = vshrq_n_s32(vshlq_n_s32(vaddq_s32(ph0, rt0), 8), 8);
        ph1 = vshrq_n_s32(vshlq_n_s32(vaddq_s32(ph1, rt1), 8), 8);
        if (--st.ncount == 0) {
            int32_t su = sext24((int32_t)st.u), au = su < 0 ? -su : su;
            int32_t r = (int32_t)st.t.io[R80_C_R1 + st.kvoice];
            st.rate[st.kvoice] = ((au & 2) ? r : -r) + (int32_t)(((int64_t)au * r + (1 << 25)) >> 26);
            st.ncount = 4096u + ((uint32_t)au >> 11);
            st.kvoice = st.kvoice == 5 ? 0 : st.kvoice + 1;
            rt0 = vld1q_s32(st.rate);
            rt1 = vld1q_s32(st.rate + 4);
        }
        /* c38 = 2^-13: the depth smoothing as a shift, 11 fraction bits. */
        unsigned k = st.ksmooth;
        st.ca_q[k] += (atgt[k] - st.ca_q[k]) >> 13;
        caf[k] = (float)st.ca_q[k] * (1.0f / 2048.0f);
        st.ksmooth = k == 5 ? 0 : k + 1;
        st.u = (st.u + weyl) & 0xffffffu;
    }
    vst1q_f32(st.cv, v0);
    vst1q_f32(st.cv + 4, v1);
    vst1q_f32(st.cm, m0v);
    vst1q_f32(st.cm + 4, m1v);
    vst1q_s32(st.phi, ph0);
    vst1q_s32(st.phi + 4, ph1);
    if (span_last && st.cglide) {
        int moving = 0;
        for (unsigned j = 0; j < 6; j++) {
            int64_t b = (int64_t)(st.t.io[R80_C_B1 + j] * 65536.0), d = b - st.cb_q[j];
            if (d > 256 || d < -256) {
                moving = 1;
            } else {
                st.cb_q[j] = b;
            }
        }
        st.cglide = (uint8_t)moving;
    }
    vst1_f32(st.la, lab);
    vst1_f32(st.ret, ret);
}

/* ---- the block ---- */

#ifdef R80_HOST
static int hold_taps, hold_sweep;
#endif

void r80_block(const int32_t (*send)[2], int32_t (*wet)[2], unsigned count)
{
    span_first = span_f == 0;
    span_f += count;
    span_last = span_f >= SPAN;
    span_f = span_last ? 0 : span_f;
    if (!span_first) {
        for (unsigned f = 0; st.p && f < count; f++) {
            float x = clipf((float)send[f][0] * SEND_SCALE), a = x < 0.0f ? -x : x;
            in_x[f] = x;
            st.peak = a > st.peak ? a : st.peak;
        }
        if (span_mute) {
            goto silent;
        }
        goto run;
    }
    span_mute = 1;
    if (hand.load_seq != st.load_seen) {
        st.load_seen = hand.load_seq;
        st.tgt_seen = hand.tgt_seq;
        load(hand.load, &hand.tgt[hand.tgt_idx]);
    }
    if (!st.p) {
        goto silent;
    }
    float peak = st.peak;
    for (unsigned f = 0; f < count; f++) {
        float x = clipf((float)send[f][0] * SEND_SCALE), a = x < 0.0f ? -x : x;
        in_x[f] = x;
        peak = a > peak ? a : peak;
    }
    st.peak = peak;
    st.ctl += SPAN;
    if (st.ctl >= CONTROL_FRAMES) {
        st.ctl -= CONTROL_FRAMES;
        control_step();
    }
    if (st.clearing && clear_step()) {
        goto silent;
    }
    if (hand.tgt_seq != st.tgt_seen) {
        st.tgt_seen = hand.tgt_seq;
        apply_target(&hand.tgt[hand.tgt_idx], 0);
    }
    if (st.sweep_tick <= SPAN) {
        st.sweep_tick += 52800u - SPAN;
        st.tstep = (uint8_t)(st.tstep == 10 ? 0 : st.tstep + 1);
        sweep_timing();
    } else {
        st.sweep_tick -= SPAN;
    }
    if (!st.npairs || !sweep_busy()) {
        st.sweep_rate = 0;
    } else if (!st.sweep_rate) {
        sweep_timing();
    }
#ifdef R80_HOST
    if (hold_sweep) {
        st.sweep_rate = 0;
    }
    if (hold_taps) {
        st.tap_rate = 0;
    }
#endif
    ramps_block();
    if (!st.fade_step && !st.restart && st.fade_pending && st.t.geometry != st.geometry) {
        st.geometry = st.t.geometry;
        st.fade_pending = 0;
        st.restart = 1;
        st.rg = 256 << 16;
    }
    if (!st.fade_step && !st.restart && st.fade_pending) {
        int any = 0;
        st.fade_pending = 0;
        for (unsigned s = 0; s < R80_K; s++) {
            fade_class[s] = !st.aowned[s] && st.a_next[s] != st.a[s];
            any |= fade_class[s];
        }
        if (any) {
            st.fade_step = -(int32_t)((256u << 16) / FADE_FRAMES);
            st.fading = 1;
        }
    }
    span_mute = 0;
run:
    if (st.algo == R80_CHORUS) {
        section31(wet, count);
    } else {
        section3(wet, count);
    }
    if (span_last && !st.fade_step) {
        st.fading = 0;
    }
    st.since += count;
    st.ready = 1;
    return;
silent:
    for (unsigned f = 0; f < count; f++) {
        wet[f][0] = wet[f][1] = 0;
    }
}

void r80_flush(void)
{
    st.load_seen = hand.load_seq;
    st.tgt_seen = hand.tgt_seq;
    load(hand.load, &hand.tgt[hand.tgt_idx]);
    while (st.p && st.clearing && clear_step()) {
    }
}

/* ---- foreground ---- */

static struct {
    const struct r80_program *p;
    struct r80_values stored, now;
    int16_t edit[R80_EDITS], src[R80_SOURCES];
    uint32_t src_seen;
    uint16_t geometry;
    uint8_t dirty;
} fg;

static void publish(const struct r80_target *t, int loading)
{
    uint8_t w = (uint8_t)(hand.tgt_idx ^ 1u);

    if (t != &hand.tgt[w]) {
        hand.tgt[w] = *t;
    }
    __sync_synchronize();
    hand.tgt_idx = w;
    if (loading) {
        hand.load_seq++;
    } else {
        hand.tgt_seq++;
    }
}

void r80_load(const struct r80_program *p)
{
    fg.p = p;
    for (unsigned e = 0; e < R80_EDITS; e++) {
        fg.edit[e] = -1;
    }
    hand.load = p;
    if (!p) {
        __sync_synchronize();
        hand.load_seq++;
        return;
    }
    for (unsigned s = 0; s < R80_SOURCES; s++) {
        fg.src[s] = 0;
    }
    fg.src[R80_SRC_NONE] = fg.src[R80_SRC_NOVALUE] = -1;
    fg.src[R80_SRC_ON] = 127;
    fg.src[R80_SRC_ADJUST] = p->adjust;
    fg.src_seen = hand.src_seq;
    fg.dirty = 0;
    r80_stored(p, &fg.stored);
    r80_patch_all(p, &fg.stored, fg.src, &fg.now);
    struct r80_target *t = &hand.tgt[hand.tgt_idx ^ 1u];
    r80_resolve(p, &fg.now, t, 1);
    t->geometry = fg.geometry;
    publish(t, 1);
}

void r80_set(unsigned e, int value)
{
    if (e < R80_EDITS && fg.edit[e] != value) {
        fg.edit[e] = (int16_t)value;
        fg.dirty = 1;
    }
}

static const uint8_t offset_params[] = { R80_P_SIZE, R80_P_PREDELAY, R80_P_REFDLY_L, R80_P_REFDLY_R, R80_P_EKODLY_L,
                                         R80_P_EKODLY_R, R80_P_SPREAD, R80_P_DURATION, R80_P_LINK };

void r80_poll(void)
{
    struct r80_values v;
    const struct r80_program *p = fg.p;

    if (!p) {
        return;
    }
    uint32_t seq = hand.src_seq;
    if (seq != fg.src_seen) {
        int16_t s[R80_SOURCES];
        do {
            seq = hand.src_seq;
            memcpy(s, hand.src[hand.src_idx], sizeof(s));
            __asm__ volatile("" ::: "memory");
        } while (seq != hand.src_seq);
        fg.src_seen = seq;
        s[R80_SRC_ADJUST] = fg.src[R80_SRC_ADJUST];
        if (memcmp(s, fg.src, sizeof(s))) {
            memcpy(fg.src, s, sizeof(s));
            fg.dirty = 1;
        }
    }
    if (!fg.dirty) {
        return;
    }
    fg.dirty = 0;
    fg.src[R80_SRC_ADJUST] = fg.edit[R80_E_ADJUST] >= 0 ? fg.edit[R80_E_ADJUST] : p->adjust;
    r80_patch_all(p, &fg.stored, fg.src, &v);
    for (unsigned e = 1; e < R80_EDITS; e++) {
        if (fg.edit[e] >= 0) {
            unsigned q = r80_edit_param(e);
            v.v[q] = fg.edit[e] * 32;
            v.locked[q >> 5] &= ~(1u << (q & 31u));
        }
    }
    if (!memcmp(&v, &fg.now, sizeof(v))) {
        return;
    }
    int offsets = 0;
    if (v.v[R80_P_SIZE] != fg.now.v[R80_P_SIZE] || v.v[R80_P_DURATION] != fg.now.v[R80_P_DURATION]) {
        fg.geometry++;
    }
    for (unsigned j = 0; j < sizeof(offset_params); j++) {
        unsigned q = offset_params[j];
        offsets |= v.v[q] != fg.now.v[q] || r80_locked(&v, q) != r80_locked(&fg.now, q);
    }
    fg.now = v;
    struct r80_target *t = &hand.tgt[hand.tgt_idx ^ 1u];
    if (!offsets) {
        memcpy(t->a, hand.tgt[hand.tgt_idx].a, sizeof(t->a));
    }
    r80_resolve(p, &v, t, offsets);
    t->geometry = fg.geometry;
    publish(t, 0);
}

const struct r80_program *r80_running(void)
{
    return fg.p;
}

int r80_live(unsigned e)
{
    if (!fg.p || e >= R80_EDITS) {
        return -1;
    }
    if (e == R80_E_ADJUST) {
        return fg.edit[e] >= 0 ? fg.edit[e] : fg.p->adjust;
    }
    return r80_plain(fg.p, &fg.now, r80_edit_param(e));
}

#ifdef R80_HOST
void r80_taps(unsigned j, unsigned *a, unsigned *b, unsigned *code)
{
    *a = st.a[tap_a[j]];
    *b = st.a[tap_b[j]];
    *code = st.code[j];
}

int r80_ready(void)
{
    return st.p && !st.clearing && st.ready && hand.load_seq == st.load_seen;
}

const struct r80_target *r80_now(void)
{
    return &st.t;
}

void r80_hold(int taps, int sweep)
{
    hold_taps = taps;
    hold_sweep = sweep;
}

const int16_t *r80_k_now(void);
const int16_t *r80_k_now(void)
{
    return st.k;
}

void r80_settle(void);
void r80_settle(void)
{
    if (hand.tgt_seq != st.tgt_seen) {
        st.tgt_seen = hand.tgt_seq;
        apply_target(&hand.tgt[hand.tgt_idx], 0);
    }
    for (unsigned j = 0; j < R80_IO_MAX; j++) {
        st.io[j] = (float)st.t.io[j];
    }
    st.since = 1u << 30;
    for (unsigned l = 0; l < 2; l++) {
        st.post.len[l] = (float)st.t.io[R80_IO_POSTL + l];
        st.v13.len[l] = (float)st.t.io[R80_IO_DV1 + 2 * l];
        st.v24.len[l] = (float)st.t.io[R80_IO_DV1 + 2 * l + 1];
        st.post.xf[l] = st.v13.xf[l] = st.v24.xf[l] = 0;
        st.post.dlen[l] = st.v13.dlen[l] = st.v24.dlen[l] = 0.0f;
    }
    for (unsigned s = 0; s < R80_K; s++) {
        if (!st.owned[s]) {
            st.k[s] = st.t.k[s];
            st.k16[s] = st.kt16[s] = st.t.k[s] * 128;
        }
        if (!st.aowned[s]) {
            st.a[s] = st.a_next[s];
        }
    }
    memset(st.ramping, 0, sizeof(st.ramping));
    st.fade_pending = st.fading = st.restart = 0;
    st.fade_step = 0;
    st.fade_g = 256 << 16;
    fade_gain[1] = 256;
    derive();
}

/* The tank and sections back to silence and their start states. */
void r80_restart(void);
void r80_restart(void)
{
    memset(mem, 0, sizeof(mem));
    memset(post_line, 0, sizeof(post_line));
    memset(voice_line, 0, sizeof(voice_line));
    memset(diff_line, 0, sizeof(diff_line));
    memset(st.s, 0, sizeof(st.s));
    st.sl = st.sr = 0;
    st.i = 0;
    st.ret[0] = st.ret[1] = st.fb[0] = st.fb[1] = st.lp[0] = st.lp[1] = 0.0f;
    st.la[0] = st.la[1] = 0.0f;
    for (unsigned j = 0; j < 6; j++) {
        st.cv[j] = st.cm[j] = 0.0f;
        st.phi[j] = 0;
        st.rate[j] = (int32_t)st.t.io[R80_C_R1 + j];
        st.cb_q[j] = (int64_t)(st.t.io[R80_C_B1 + j] * 65536.0);
        st.ca_q[j] = (int32_t)st.t.io[R80_C_A1 + j] << 11;
    }
    st.u = (uint32_t)st.t.io[R80_C_U0];
    st.ncount = (uint32_t)st.t.io[R80_C_N];
    st.kvoice = st.ksmooth = 0;
}
#endif
