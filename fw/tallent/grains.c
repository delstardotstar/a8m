#include "grains.h"
#include "eq.h"

#include <arm_neon.h>

/* Grain mode: one channel, or a channel and its Stereo/Right partner, plays
 * overlapping windowed grains of its sample in place of its ordinary voice.
 *
 * The voice still runs and stays in charge. A grain starts where the
 * voice's playhead is, at the voice's pitch (keyboard, pitch CV and FM
 * included), and keeps that pitch for its life. The grain mix replaces the
 * voice's rendered sample before channel effects, so the envelope, level,
 * Bits, EQ, pan and mix all apply. Scan is how fast the start moves through
 * the zone against the voice: 100 follows the playhead, 0 holds where the
 * note began, and anything between stretches time at the voice's pitch.
 * Shape and Skew make each grain's window; on a pair the grains spread
 * across the stereo field as they grow more random. With every grain
 * sounding, a new one takes the one nearest its end and the next nearest
 * fades out ahead of the grain after. */

#define SCAN_FOLLOW            100         /* the start keeps to the playhead */
#define SCAN_STEP_SHIFT        16u
#define TURN_MARGIN            64u         /* frames before the loop's end that count as the loop coming round */
#define OUTPUT_FRAMES_PER_MS   48u
#define TENTHS_PER_MS          10u
#define PERCENT                100u
#define CENTS_PER_OCTAVE_Q30   894785      /* 2^30 / 1200 */
#define ONE_Q30                (1u << 30)
#define ONE_Q16                (1u << 16)
#define ONE_Q15                (1 << 15)
#define INTERVAL_SHIFT         8u          /* grain interval: Q8 frames */
#define SAMPLE_SHIFT           4u          /* Q31 PCM to Q27: room for the cubic and eight grains */
#define EDGE_SHIFT             12u         /* window fade arithmetic: Q20 of the grain */
#define WHOLE_GRAIN            (1ull << 32) /* a grain's phase, end to end */
/* The window (after Mutable Instruments Beads' grain envelope, MIT): Shape
 * is the share of the grain spent fading, Skew splits it between the
 * attack and the release. At Skew 0 both fades are smooth; toward either
 * end the attack turns linear and the release a fourth-power tail, so
 * -100 is a struck decay and +100 a swell that cuts off. Each fade has a
 * floor so a short grain does not click, longer for a sharp tail. */
#define SKEW_FULL              100
#define ATTACK_MIN_FRAMES      24u         /* 0.5 ms */
#define RELEASE_MIN_FRAMES     24u         /* 0.5 ms, a smooth release */
#define RELEASE_SHARP_FRAMES   480u        /* 10 ms, a fourth-power release */
#define SMOOTH_ONE             (1 << 15)   /* Q15: all smooth fade */
/* Grains stolen to make room fade out over at most this, and by the next grain. */
#define STEAL_FADE_FRAMES      240u        /* 5 ms */
#define RATE_MIN               (1 << 10)   /* below this the voice is not moving */
#define RATE_MAX               (1 << 29)   /* 16 frames per frame */
#define SPAN_MARGIN            4u          /* frames the cubic reads around a grain */
#define DRIFT_REWRAP_LIMIT     4u          /* zone lengths of Scan drift folded by subtraction */
#define DETUNES                GRAIN_DETUNES
/* Pan spread widens with the grains' randomness (after Beads): Spray
 * against the grain's length, Detune against an octave, and Octave and
 * Reverse by how evenly they split the grains; their sum, at most one,
 * times half the grains sounding is the spread. Pans gather toward the
 * middle. A panned grain keeps its near side and crosses the far side's
 * signal over at about equal power. */
#define DETUNE_SCATTER_CENTS   1200
#define FRACTION_ONE_Q16       (1u << 16)
#define RANDOM_SEED            0x2545f491u
#define SEED_STRIDE            0x9e3779b9u /* local: each channel its own sequence */
#define AUDIO_STEP_SHIFT       GRAIN_STEP_SHIFT
#define CACHE_LINE_BYTES       64u

#define SLIDE_WHOLE            1000        /* Slide's top: the zone's length */
#define NORM_GLIDE_SHIFT       6u          /* the output gain reaches a new value over ~64 frames */
/* What a route's CV adds at +1.00 (EQ_CV_AMOUNT_MAX): five volts sweep the
 * field's span, or for Size and Overlap five octaves. Slide's span is the
 * zone's length and it goes round the zone. */
#define ROUTE_VOLTS            5
#define ROUTE_CENTS_PER_VOLT   1200
#define ROUTE_COUNT_SCALE      (EQ_CV_COUNTS_FULL * EQ_CV_AMOUNT_MAX)
const struct grain_route grain_routes[GRAIN_ROUTES] = {
    [GRAIN_ROUTE_SIZE] = {GRAIN_SIZE, 0}, [GRAIN_ROUTE_OVERLAP] = {GRAIN_OVERLAP, 0},
    [GRAIN_ROUTE_SPRAY] = {GRAIN_SPRAY, 2000}, [GRAIN_ROUTE_DETUNE] = {GRAIN_DETUNE, 1200},
    [GRAIN_ROUTE_SLIDE] = {GRAIN_SLIDE, SLIDE_WHOLE}, [GRAIN_ROUTE_SCAN] = {GRAIN_SCAN, 800},
    [GRAIN_ROUTE_SHAPE] = {GRAIN_SHAPE, 100}, [GRAIN_ROUTE_PAN] = {GRAIN_PAN, 100},
};

/* 2^(cents/1200) as Q16, within an octave either way. */
u32 grain_ratio_q16(s32 cents)
{
    static const u32 c[] = {10322906u, 59597033u, 257940273u, 744261118u}; /* ln2^k/k!, Q30 */
    s32 x = cents * CENTS_PER_OCTAVE_Q30, whole = x >> 30;
    u32 f = (u32)(x - whole * (s32)ONE_Q30);
    u64 y = c[0];
    for (u32 i = 1; i < 4; ++i) y = c[i] + ((y * f) >> 30);
    u32 mantissa = ONE_Q30 + (u32)((y * f) >> 30);
    return mantissa >> (30 - 16 - whole);
}

/* The whole square root. */
static u32 root(u32 x)
{
    u32 r = 0;
    for (u32 bit = 1u << 15; bit; bit >>= 1) if ((r | bit) * (r | bit) <= x) r |= bit;
    return r;
}

/* The reciprocal that turns a phase within a fade into Q30 of it. */
static u32 inverse(u32 reach)
{
    u32 r = reach >> EDGE_SHIFT;
    return ONE_Q30 / (r ? r : 1u);
}

/* The window's fades as phases: Shape's share of the grain, split by Skew,
 * each at least its floor; a grain shorter than its floors is all fade. */
static struct grain_fades fades_for(u32 phase_step, u32 shape, s32 skew)
{
    struct grain_fades f;
    u32 sharp = (u32)(skew < 0 ? -skew : skew);
    u64 fading = WHOLE_GRAIN * shape / PERCENT;
    u64 attack = fading * (u32)(SKEW_FULL + skew) / (2 * SKEW_FULL), release = fading - attack;
    u64 attack_min = (u64)ATTACK_MIN_FRAMES * phase_step;
    u64 release_min = (u64)(RELEASE_MIN_FRAMES + (RELEASE_SHARP_FRAMES - RELEASE_MIN_FRAMES) * sharp / SKEW_FULL) * phase_step;
    if (attack < attack_min) attack = attack_min;
    if (release < release_min) release = release_min;
    if (attack + release >= WHOLE_GRAIN) {
        /* Each fade keeps its smooth floor, or half a grain shorter than two. */
        u64 sum = attack + release, half = WHOLE_GRAIN / 2u;
        u64 attack_keep = attack_min < half ? attack_min : half;
        u64 release_keep = (u64)RELEASE_MIN_FRAMES * phase_step;
        if (release_keep > half) release_keep = half;
        attack = attack * (WHOLE_GRAIN - 1u) / sum;
        if (attack < attack_keep) attack = attack_keep;
        if (WHOLE_GRAIN - 1u - attack < release_keep) attack = WHOLE_GRAIN - 1u - release_keep;
        release = WHOLE_GRAIN - 1u - attack;
    }
    f.attack = (u32)attack; f.attack_inverse = inverse(f.attack);
    f.release = (u32)release; f.release_inverse = inverse(f.release);
    f.smooth = (u32)(SKEW_FULL - (s32)sharp) * SMOOTH_ONE / SKEW_FULL;
    return f;
}

/* The window's mean and mean square, Q16 of a grain at full level, for the
 * output gain: full between the fades, and each fade blends its base curve
 * with the smooth one by s. Attack t: mean 1/2, mean square 1/3 + s/30 +
 * s^2/210. Release t^4: mean 1/5 + 3s/10, mean square 1/9 + 17s/126 +
 * 79s^2/630. */
static void window_moments(const struct grain_fades *f, u32 *mean, u32 *square)
{
    u32 a = f->attack >> 16, r = f->release >> 16, s = f->smooth << 1, s2 = (u32)(((u64)s * s) >> 16);
    u32 flat = FRACTION_ONE_Q16 - a - r;
    u32 release_mean = FRACTION_ONE_Q16 / 5u + 3u * s / 10u;
    u32 attack_square = FRACTION_ONE_Q16 / 3u + s / 30u + s2 / 210u;
    u32 release_square = FRACTION_ONE_Q16 / 9u + 17u * s / 126u + 79u * s2 / 630u;
    *mean = flat + a / 2u + (u32)(((u64)r * release_mean) >> 16);
    *square = flat + (u32)(((u64)a * attack_square) >> 16) + (u32)(((u64)r * release_square) >> 16);
}

/* How even a percent's split of the grains is, Q16: 4p(1 - p). */
static u32 split(u32 percent) { return 4u * percent * (PERCENT - percent) * FRACTION_ONE_Q16 / (PERCENT * PERCENT); }

/* The per-frame values of the fields. */
void grain_derive(const s32 *values, struct grain_tuning *t)
{
    struct grain_tuning next;
    u32 frames = (u32)values[GRAIN_SIZE] * OUTPUT_FRAMES_PER_MS / TENTHS_PER_MS;
    u32 overlap = (u32)values[GRAIN_OVERLAP];
    next.frames = frames;
    next.phase_step = ~0u / frames;
    next.interval = (frames << INTERVAL_SHIFT) * PERCENT / overlap;
    next.fades = fades_for(next.phase_step, (u32)values[GRAIN_SHAPE], values[GRAIN_SKEW]);
    /* Grains that line up add in amplitude: overlap x the window's mean.
     * Grains that read different sound add in power: overlap x its mean
     * square. Each is brought back to the voice's level. A pair's pans keep
     * about equal power, and grains that line up are never panned. */
    u32 mean, square;
    window_moments(&next.fades, &mean, &square);
    u32 cover = overlap * mean / PERCENT, power = overlap * square / PERCENT;
    int scattered = values[GRAIN_SPRAY] || values[GRAIN_DETUNE] || values[GRAIN_OCTAVE] ||
                    values[GRAIN_REVERSE] || values[GRAIN_SCAN] != SCAN_FOLLOW;
    if (scattered) next.norm = power <= FRACTION_ONE_Q16 ? ONE_Q16 : (1u << 24) / root(power);
    else next.norm = cover <= ONE_Q16 ? ONE_Q16 : ~0u / cover;
    next.spray = (u32)values[GRAIN_SPRAY] * OUTPUT_FRAMES_PER_MS;
    next.octave = (u32)values[GRAIN_OCTAVE] * 256u / PERCENT;
    next.reverse = (u32)values[GRAIN_REVERSE] * 256u / PERCENT;
    u64 spray_share = (u64)next.spray * FRACTION_ONE_Q16 / frames;
    u64 scatter = (spray_share < FRACTION_ONE_Q16 ? spray_share : FRACTION_ONE_Q16) +
                  (u32)values[GRAIN_DETUNE] * FRACTION_ONE_Q16 / DETUNE_SCATTER_CENTS +
                  split((u32)values[GRAIN_OCTAVE]) + split((u32)values[GRAIN_REVERSE]);
    next.scatter = scatter < FRACTION_ONE_Q16 ? (u32)scatter : FRACTION_ONE_Q16;
    next.pan = values[GRAIN_PAN] * ONE_Q15 / (s32)PERCENT;
    next.backwards = values[GRAIN_SCAN] < 0;
    next.slide = values[GRAIN_SLIDE] * (s32)ONE_Q16 / SLIDE_WHOLE;
    next.scan = (values[GRAIN_SCAN] - SCAN_FOLLOW) * (1 << SCAN_STEP_SHIFT) / (s32)PERCENT;
    for (u32 i = 0; i < DETUNES; ++i)
        next.detune[i] = grain_ratio_q16(values[GRAIN_DETUNE] * (2 * (s32)i - (s32)(DETUNES - 1)) / (s32)(DETUNES - 1));
    *t = next;
}

/* `value` moved by cents, any number of octaves. */
static s32 scaled(s32 value, s32 cents)
{
    s32 octaves = cents / ROUTE_CENTS_PER_VOLT, rest = cents % ROUTE_CENTS_PER_VOLT;
    s64 wide = ((s64)value * grain_ratio_q16(rest)) >> 16;
    return (s32)(octaves < 0 ? wide >> -octaves : wide << octaves);
}

/* A channel's fields with its CV routes applied. */
void grain_fields(const struct grain_value *g, u32 channel, s32 *next)
{
    for (u32 f = 0; f < GRAIN_FIELDS; ++f) next[f] = g->value[f];
    for (u32 r = 0; r < GRAIN_ROUTES; ++r) {
        u32 f = grain_routes[r].field;
        s32 low = grain_ranges[f].low, high = grain_ranges[f].high;
        s64 turn = (s64)eq_cv_counts(g->source[r], channel) * g->amount[r] * ROUTE_VOLTS;
        if (!turn) continue;
        if (!grain_routes[r].span) next[f] = scaled(next[f], (s32)(turn * ROUTE_CENTS_PER_VOLT / ROUTE_COUNT_SCALE));
        else next[f] += (s32)(turn * grain_routes[r].span / (ROUTE_COUNT_SCALE * ROUTE_VOLTS));
        if (f == GRAIN_SLIDE) next[f] %= SLIDE_WHOLE;
        next[f] = next[f] < low ? low : next[f] > high ? high : next[f];
    }
}

/* A start in the zone after Slide, wrapped to the zone. `frame` is within a
 * zone's length of it either way. */
static s32 placed(const struct grain_tuning *t, s32 frame, const struct grain_zone *r)
{
    s32 length = (s32)r->frames;
    frame += (s32)(((s64)length * t->slide) >> 16);
    while (frame < 0) frame += length;
    while (frame >= length) frame -= length;
    return frame;
}

static u32 random(struct grain_state *s)
{
    u32 seed = s->seed;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    s->seed = seed;
    return seed;
}

/* A pan's gains for a pair, Q15 [out][in]: the near side keeps its own
 * signal and takes the far side's at 1 - (1 - |d|)^2; the far side keeps
 * 1 - d^2 of its own. */
static void pan_gains(s32 gain[2][2], s32 d)
{
    s32 m = d < 0 ? -d : d, far = ONE_Q15 - (s32)(((s64)m * m) >> 15), cross = ONE_Q15 - (s32)(((s64)(ONE_Q15 - m) * (ONE_Q15 - m)) >> 15);
    u32 near = d < 0 ? 0u : 1u;
    gain[near][near] = ONE_Q15; gain[near][near ^ 1u] = d ? cross : 0;
    gain[near ^ 1u][near ^ 1u] = d ? far : ONE_Q15; gain[near ^ 1u][near] = 0;
}

/* `due` is how long ago the grain should have started, Q8 frames. */
static void spawn(struct grain_state *s, const struct grain_tuning *t, struct grain *g, const struct grain_zone *r,
                  u32 at, s32 step, u32 sounding, u32 due)
{
    u32 chance = random(s);
    s64 wide = ((s64)(step < 0 ? -step : step) * (s32)t->detune[chance % DETUNES]) >> 16;
    if (((chance >> 8) & 0xffu) < t->octave) wide *= 2;
    s32 rate = wide > RATE_MAX ? RATE_MAX : (s32)wide;
    u32 span = (u32)(((u64)t->frames * (u32)rate) >> AUDIO_STEP_SHIFT) + SPAN_MARGIN;
    /* A voice that is not playing parks its playhead past the sample. */
    if (at >= r->frames || rate < RATE_MIN || span >= r->frames) { ++s->skipped; return; }

    /* Scan drift moves the start around the zone; Spray scatters it. */
    s32 length = (s32)r->frames, moved = (s32)(s->drift >> 32);
    if (moved >= length || moved <= -length) {
        if (moved >= (s32)DRIFT_REWRAP_LIMIT * length || moved <= -(s32)DRIFT_REWRAP_LIMIT * length) s->drift = 0;
        while ((s32)(s->drift >> 32) >= length) s->drift -= (s64)length << 32;
        while ((s32)(s->drift >> 32) <= -length) s->drift += (s64)length << 32;
        moved = (s32)(s->drift >> 32);
    }
    s32 start = placed(t, (s32)at + moved, r);
    if (t->spray) start += (s32)(((u64)random(s) * t->spray) >> 32) - (s32)(t->spray >> 1);
    s32 last = length - (s32)span;
    if (start < 1) start = 1;
    if (start > last) start = last;

    g->pcm = r->pcm; g->right = r->right; g->dir = r->dir;
    if (((chance >> 16) & 0xffu) < t->reverse) {
        g->position = (s64)(start + (s32)(span - SPAN_MARGIN)) << 32;
        g->step = -rate;
    } else {
        g->position = (s64)start << 32;
        g->step = rate;
    }
    /* The spread is the grains' randomness times half of them sounding,
     * this one counted; a second random draw gathers pans to the middle. */
    u32 pick = random(s), spread = (sounding + 1u) * t->scatter / 2u;
    u32 reach = (u32)(((u64)spread * (pick >> 16)) >> 16);
    if (reach > FRACTION_ONE_Q16) reach = FRACTION_ONE_Q16;
    s32 d = (s32)((((s64)((s32)(pick & 0xffffu) - ONE_Q15) * (s32)reach) >> 16) * t->pan >> 15);
    pan_gains(g->gain, d);
    g->position += (s64)g->step * (1 << (32u - AUDIO_STEP_SHIFT)) * due >> INTERVAL_SHIFT;
    g->phase_step = t->phase_step;
    g->phase = (g->phase_step >> INTERVAL_SHIFT) * due;
    g->fades = t->fades;
    g->fade = ONE_Q30; g->fade_step = 0;
    ++s->spawned;
}

/* Local: the grains render a block at a time, four sounding grains to a
 * vector, between the frames that start one; upstream's per-frame
 * arithmetic, lane by lane (the same results, tools/graintest.sh). */
#define BLOCK_MAX 32u

/* The Q27 grain sum at unity, as a saturated Q31 sample. */
static s32 output(s64 sum, s32 norm)
{
    s64 wide = (sum * norm) >> (16u - SAMPLE_SHIFT);
    return wide > 0x7fffffff ? 0x7fffffff : wide < -0x7fffffff - 1 ? -0x7fffffff - 1 : (s32)wide;
}

void grain_start(struct grain_state *s, const struct grain_tuning *t, u32 channel)
{
    memset(s, 0, sizeof(*s));
    s->seed = RANDOM_SEED + channel * SEED_STRIDE;
    s->norm = (s32)t->norm;
    s->heard = GRAIN_PARKED;
}

/* x * y >> 30 for four u32 lanes, as upstream's 64-bit products. */
static inline uint32x4_t mul_u30(uint32x4_t x, uint32x4_t y)
{
    return vcombine_u32(vshrn_n_u64(vmull_u32(vget_low_u32(x), vget_low_u32(y)), 30),
                        vshrn_n_u64(vmull_u32(vget_high_u32(x), vget_high_u32(y)), 30));
}

/* The same for s32 lanes, >> 30 or >> 15. */
static inline int32x4_t mul_s30(int32x4_t x, int32x4_t y)
{
    return vcombine_s32(vshrn_n_s64(vmull_s32(vget_low_s32(x), vget_low_s32(y)), 30),
                        vshrn_n_s64(vmull_s32(vget_high_s32(x), vget_high_s32(y)), 30));
}

static inline int32x4_t mul_s15(int32x4_t x, int32x4_t y)
{
    return vcombine_s32(vshrn_n_s64(vmull_s32(vget_low_s32(x), vget_low_s32(y)), 15),
                        vshrn_n_s64(vmull_s32(vget_high_s32(x), vget_high_s32(y)), 15));
}

/* Four-point cubic through a b c d (Q31) at fraction f (Q31 of a frame). */
static inline int32x4_t interpolate(int32x4_t a, int32x4_t b, int32x4_t c, int32x4_t d, int32x4_t f)
{
    a = vshrq_n_s32(a, SAMPLE_SHIFT); b = vshrq_n_s32(b, SAMPLE_SHIFT);
    c = vshrq_n_s32(c, SAMPLE_SHIFT); d = vshrq_n_s32(d, SAMPLE_SHIFT);
    int32x4_t c1 = vshrq_n_s32(vsubq_s32(c, a), 1);
    int32x4_t c2 = vsubq_s32(vaddq_s32(vsubq_s32(vsubq_s32(a, vshlq_n_s32(b, 1)), vshrq_n_s32(b, 1)), vshlq_n_s32(c, 1)),
                             vshrq_n_s32(d, 1));
    int32x4_t bc = vsubq_s32(b, c);
    int32x4_t c3 = vaddq_s32(vaddq_s32(vshrq_n_s32(vsubq_s32(d, a), 1), bc), vshrq_n_s32(bc, 1));
    int32x4_t y = vaddq_s32(vqdmulhq_s32(c3, f), c2);
    y = vaddq_s32(vqdmulhq_s32(y, f), c1);
    return vaddq_s32(vqdmulhq_s32(y, f), b);
}

/* The window at four phases: an attack line or a fourth-power release,
 * blended with the smooth fade; times the steal fade. */
static inline uint32x4_t window_fade(uint32x4_t x, uint32x4_t base, int32x4_t smooth, uint32x4_t fade)
{
    uint32x4_t square = mul_u30(x, x);
    uint32x4_t smoothed = mul_u30(square, vsubq_u32(vdupq_n_u32(3u * ONE_Q30), vshlq_n_u32(x, 1)));
    int32x4_t d = vreinterpretq_s32_u32(vsubq_u32(smoothed, base));
    int32x4_t blend = mul_s15(d, smooth);
    return mul_u30(vaddq_u32(base, vreinterpretq_u32_s32(blend)), fade);
}

static inline uint32x4_t attack_x(uint32x4_t phase, uint32x4_t inverse)
{
    return vmulq_u32(vshrq_n_u32(phase, EDGE_SHIFT), inverse);
}

static inline uint32x4_t release_x(uint32x4_t phase, uint32x4_t inverse)
{
    return vmulq_u32(vshrq_n_u32(vmvnq_u32(phase), EDGE_SHIFT), inverse);
}

static inline uint32x4_t fourth(uint32x4_t x)
{
    uint32x4_t square = mul_u30(x, x);
    return mul_u30(square, square);
}

/* acc[0..3] += four lanes. */
static inline void add4(s64 *acc, int64x2_t lo, int64x2_t hi)
{
    vst1q_s64(acc, vaddq_s64(vld1q_s64(acc), lo));
    vst1q_s64(acc + 2, vaddq_s64(vld1q_s64(acc + 2), hi));
}

/* Frames [from, to) of a sounding grain into the sums, four frames to a
 * vector; it ends where upstream's would. */
static inline __attribute__((always_inline)) void render_grain(struct grain *g, const u32 pair, u32 from, u32 to,
                                                               s64 *left, s64 *right)
{
    const struct grain_fades *f = &g->fades;
    /* Local: only a grain ending in this span divides (the A8 has no divider). */
    u32 frames = to - from, life = frames + 1u;
    if ((u64)frames * g->phase_step > ~g->phase) life = 1u + (~g->phase) / g->phase_step;
    if (g->fade_step && (u64)frames * g->fade_step >= g->fade) {
        u32 faded = (g->fade + g->fade_step - 1u) / g->fade_step;
        if (faded < life) life = faded;
    }
    u32 n = life < frames ? life : frames;
    const s32 *pcm = g->pcm;
    const s32 dir = g->dir, side = g->right;
    const int32x4_t smooth = vdupq_n_s32((s32)f->smooth);
    const u32 attack = f->attack, release = f->release, phase_step = g->phase_step;
    const uint32x4_t attack_inverse = vdupq_n_u32(f->attack_inverse), release_inverse = vdupq_n_u32(f->release_inverse);
    const int32x4_t g00 = vdupq_n_s32(g->gain[0][0]), g01 = vdupq_n_s32(g->gain[0][1]);
    const int32x4_t g10 = vdupq_n_s32(g->gain[1][0]), g11 = vdupq_n_s32(g->gain[1][1]);
    const s64 dpos = (s64)g->step * (1 << (32u - AUDIO_STEP_SHIFT));
    const s32 ahead = (g->step < 0) != (dir < 0) ? -(s32)(CACHE_LINE_BYTES / 4) : (s32)(CACHE_LINE_BYTES / 4);
    const uint32x4_t lane = {0, 1, 2, 3};
    uint32x4_t phase = vmlaq_n_u32(vdupq_n_u32(g->phase), lane, g->phase_step);
    uint32x4_t fade = vmlsq_n_u32(vdupq_n_u32(g->fade), lane, g->fade_step);
    const uint32x4_t phase4 = vdupq_n_u32(4u * g->phase_step), fade4 = vdupq_n_u32(4u * g->fade_step);
    const int64x2_t d2 = vdupq_n_s64(2 * dpos), d4 = vdupq_n_s64(4 * dpos);
    int64x2_t pos0 = vcombine_s64(vcreate_s64((u64)g->position), vcreate_s64((u64)(g->position + dpos)));
    int64x2_t pos1 = vaddq_s64(pos0, d2);
    s64 pos = g->position;
    u32 ph = g->phase;

    for (u32 k = 0; k < n; k += 4) {
        u32 count = n - k < 4u ? n - k : 4u;
        s64 at = pos;
        const s32 *q = pcm + (s32)(at >> 32) * dir;
        int32x4_t a = vld1q_dup_s32(q - dir), b = vld1q_dup_s32(q), c = vld1q_dup_s32(q + dir), d = vld1q_dup_s32(q + 2 * dir);
        int32x4_t ra = a, rb = b, rc = c, rd = d;
        if (pair) {
            ra = vld1q_dup_s32(q + side - dir); rb = vld1q_dup_s32(q + side);
            rc = vld1q_dup_s32(q + side + dir); rd = vld1q_dup_s32(q + side + 2 * dir);
        }
        if (count > 1u) at += dpos;
#define LANE(l) { \
            q = pcm + (s32)(at >> 32) * dir; \
            a = vld1q_lane_s32(q - dir, a, l); b = vld1q_lane_s32(q, b, l); \
            c = vld1q_lane_s32(q + dir, c, l); d = vld1q_lane_s32(q + 2 * dir, d, l); \
            if (pair) { \
                q += side; \
                ra = vld1q_lane_s32(q - dir, ra, l); rb = vld1q_lane_s32(q, rb, l); \
                rc = vld1q_lane_s32(q + dir, rc, l); rd = vld1q_lane_s32(q + 2 * dir, rd, l); \
            } \
            if (l + 1u < count) at += dpos; }
        __builtin_prefetch(pcm + (s32)(pos >> 32) * dir + ahead);
        LANE(1) LANE(2) LANE(3)
#undef LANE
        pos += 4 * dpos;
        int32x4_t fr = vreinterpretq_s32_u32(vshrq_n_u32(vcombine_u32(vmovn_u64(vreinterpretq_u64_s64(pos0)),
                                                                      vmovn_u64(vreinterpretq_u64_s64(pos1))), 1));
        pos0 = vaddq_s64(pos0, d4);
        pos1 = vaddq_s64(pos1, d4);
        u32 last = ph + (count - 1u) * phase_step;
        uint32x4_t w;
        if (last < attack) w = window_fade(attack_x(phase, attack_inverse), attack_x(phase, attack_inverse), smooth, fade);
        else if (ph >= attack && ~last >= release) w = fade;
        else if (ph >= attack && ~ph < release) {
            uint32x4_t x = release_x(phase, release_inverse);
            w = window_fade(x, fourth(x), smooth, fade);
        } else {
            uint32x4_t in_attack = vcltq_u32(phase, vdupq_n_u32(attack));
            uint32x4_t in_release = vbicq_u32(vcltq_u32(vmvnq_u32(phase), vdupq_n_u32(release)), in_attack);
            uint32x4_t x = vbslq_u32(in_attack, attack_x(phase, attack_inverse), release_x(phase, release_inverse));
            w = window_fade(x, vbslq_u32(in_attack, x, fourth(x)), smooth, fade);
            w = vbslq_u32(vorrq_u32(in_attack, in_release), w, fade);
        }
        int32x4_t live = vreinterpretq_s32_u32(vcltq_u32(lane, vdupq_n_u32(count)));
        int32x4_t ws = vandq_s32(vreinterpretq_s32_u32(w), live);
        int32x4_t sample = mul_s30(interpolate(a, b, c, d, fr), ws);
        if (!pair) {
            add4(left + from + k, vmovl_s32(vget_low_s32(sample)), vmovl_s32(vget_high_s32(sample)));
        } else {
            int32x4_t other = mul_s30(interpolate(ra, rb, rc, rd, fr), ws);
            add4(left + from + k,
                 vshrq_n_s64(vmlal_s32(vmull_s32(vget_low_s32(sample), vget_low_s32(g00)), vget_low_s32(other), vget_low_s32(g01)), 15),
                 vshrq_n_s64(vmlal_s32(vmull_s32(vget_high_s32(sample), vget_high_s32(g00)), vget_high_s32(other), vget_high_s32(g01)), 15));
            add4(right + from + k,
                 vshrq_n_s64(vmlal_s32(vmull_s32(vget_low_s32(sample), vget_low_s32(g10)), vget_low_s32(other), vget_low_s32(g11)), 15),
                 vshrq_n_s64(vmlal_s32(vmull_s32(vget_high_s32(sample), vget_high_s32(g10)), vget_high_s32(other), vget_high_s32(g11)), 15));
        }
        phase = vaddq_u32(phase, phase4);
        fade = vsubq_u32(fade, fade4);
        ph += 4u * phase_step;
    }
    if (life <= frames) { g->phase_step = 0; return; }
    g->position += (s64)frames * dpos;
    g->phase += frames * g->phase_step;
    g->fade -= frames * g->fade_step;
}

/* Local: each grain renders from where it was last brought up to (done)
 * only when a spawn changes it, else once at the block's end; a spawn
 * decides on where the others would be (the same results). */
static void catch_up(struct grain *g, u32 pair, u32 *done, u32 to, s64 *left, s64 *right)
{
    if (g->phase_step && *done < to) {
        if (pair) render_grain(g, 1, *done, to, left, right);
        else render_grain(g, 0, *done, to, left, right);
    }
    *done = to;
}

/* A grain's phase k frames past where it was rendered to; 0 if it ends by then. */
static u32 phase_after(const struct grain *g, u32 k, u32 *phase)
{
    if (!g->phase_step || (u64)k * g->phase_step > ~g->phase || (g->fade_step && (u64)k * g->fade_step >= g->fade)) return 0;
    *phase = g->phase + k * g->phase_step;
    return 1;
}

/* Scan's drift and the countdown to the next grain, frame by frame; the
 * grains between the frames that start one. */
void grain_render(struct grain_state *s, const struct grain_tuning *t, const struct grain_zone *r, int pair,
                  const u32 *at, const s32 *steps, s32 *out_left, s32 *out_right, u32 frames)
{
    s64 left[BLOCK_MAX + 4] __attribute__((aligned(16))), right[BLOCK_MAX + 4] __attribute__((aligned(16)));
    memset(left, 0, (frames + 4u) * sizeof(left[0]));
    if (pair) memset(right, 0, (frames + 4u) * sizeof(right[0]));
    u32 done[GRAINS] = {0};

    for (u32 f = 0; f < frames; ++f) {
        s32 step = steps[f];
        /* Scan: the start falls behind the playhead, or runs ahead of it, by a
         * share of the voice's step, and goes round the zone's loop as the voice
         * does. A note starts it again from the playhead. */
        if (at[f] < r->frames) {
            s64 round = (s64)(r->turn - r->loop) << 32;
            u32 reach = (u32)((step < 0 ? -step : step) >> AUDIO_STEP_SHIFT) + TURN_MARGIN;
            if (s->heard >= r->frames || (at[f] < s->heard && s->heard + reach < r->turn)) s->drift = 0;
            else {
                if (at[f] < s->heard) s->drift += round;
                s->drift += ((s64)step * t->scan) >> (SCAN_STEP_SHIFT + AUDIO_STEP_SHIFT - 32u);
                s32 place = (s32)at[f] + (s32)(s->drift >> 32);
                if (place >= (s32)r->turn) s->drift -= round;
                else if (t->backwards && place < (s32)r->loop) s->drift += round;
            }
        }
        s->heard = at[f];
        s->countdown -= 1 << INTERVAL_SHIFT;
        if (s->countdown > 0) continue;
        /* A free grain, or else the one nearest its end; then, with none
         * left free, the next nearest fades out in time for the next grain. */
        u32 phase[GRAINS], live = 0;
        int taken = -1, oldest = -1;
        u32 sounding = 0;
        for (u32 n = 0; n < GRAINS; ++n) {
            if (!phase_after(&s->grains[n], f - done[n], &phase[n])) { if (taken < 0) taken = (int)n; continue; }
            live |= 1u << n;
            ++sounding;
            if (oldest < 0 || phase[n] > phase[oldest]) oldest = (int)n;
        }
        if (taken < 0) { taken = oldest; --sounding; ++s->stolen; }
        u32 due = (u32)-s->countdown;
        s->countdown += (s32)t->interval;
        catch_up(&s->grains[taken], (u32)pair, &done[taken], f, left, right);
        spawn(s, t, &s->grains[taken], r, at[f], step, sounding, due);
        if (s->grains[taken].phase_step && sounding + 1u >= GRAINS) {
            int next_out = -1;
            for (u32 n = 0; n < GRAINS; ++n) {
                if ((int)n != taken && (live >> n) & 1 && !s->grains[n].fade_step && (next_out < 0 || phase[n] > phase[next_out])) next_out = (int)n;
            }
            u32 fade = t->interval >> INTERVAL_SHIFT;
            if (fade > STEAL_FADE_FRAMES) fade = STEAL_FADE_FRAMES;
            if (next_out >= 0 && ~phase[next_out] / s->grains[next_out].phase_step > fade) {
                catch_up(&s->grains[next_out], (u32)pair, &done[next_out], f, left, right);
                s->grains[next_out].fade_step = ONE_Q30 / (fade ? fade : 1u);
                ++s->stolen;
            }
        }
    }
    for (u32 n = 0; n < GRAINS; ++n) catch_up(&s->grains[n], (u32)pair, &done[n], frames, left, right);
    for (u32 f = 0; f < frames; ++f) {
        /* The output gain follows Overlap, Shape and Pan without a step. */
        s->norm += ((s32)t->norm - s->norm) >> NORM_GLIDE_SHIFT;
        out_left[f] = output(left[f], s->norm);
        if (pair) out_right[f] = output(right[f], s->norm);
    }
}
