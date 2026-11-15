/* Local: upstream's ext/mix_fx.c with its glue to the stock firmware
 * replaced by the block entries at the end (UPSTREAM lists the changes). */
#include "audio.h"
#include "mix_fx.h"
#include "mix_tables.h"
#include "prof.h"

#include <arm_neon.h>

/* Mix effects in the audio interrupt (upstream integer only; here with
 * NEON, UPSTREAM). Levels and gain reductions are log2 values in Q24
 * (1 << 24 is an octave, 6.02 dB). */

#define Q24_OCTAVE         (1 << 24)
#define DB_TO_Q24(db)      ((s32)((db) * 2786918))        /* 2^24 / 6.0206 */
#define LEVEL_FLOOR        (-24 * Q24_OCTAVE)
#define GAIN_ONE_Q24       (1 << 24)
#define FRAME_HZ           48000u
#define REVERB_HZ          32000u
#define REVERB_WORDS       32768u
#define REVERB_MASK        (REVERB_WORDS - 1u)
#define DELAY_WORDS        65536u
#define DELAY_MASK         (DELAY_WORDS - 1u)
#define DELAY_MIN_FRAMES   64u
#define DELAY_GLIDE_SHIFT  11      /* the delay time follows a change over about 40 ms */
#define DIVISION_UNITS     48u     /* a division counts 48ths of a beat */
#define DELAY_RETARGET_MASK 255u   /* the delay follows the clock every 256 frames */
#define SOFT_CLIP_LIMIT    49152   /* 1.5 in Q15: the delay loop's clip reaches 1.0 here */
#define PERCENT_Q15        328     /* 1 % in Q15 */
#define Q15_ONE            32768
#define CHANNEL_SHIFT      1       /* channels arrive at 2^30, the mix at 2^31 (Q31 full scale) */

struct mix_settings mix_defaults;

void mix_channel_get(const struct mix_settings *from, u32 c, struct mix_channel *out)
{
    out->comp = from->comp[c];
    out->reverb_send = from->reverb_send[c];
    out->delay_send = from->delay_send[c];
    out->send_pre = from->send_pre[c];
    memcpy(out->send_route, from->send_route[c], sizeof(out->send_route));
}
/* Unchanged settings stay as they are, so a copy of defaults leaves a
 * preset without its own settings unmodified. */
void mix_channel_put(u32 c, const struct mix_channel *in)
{
    struct mix_channel now;
    mix_channel_get(mix_now(), c, &now);
    if (!memcmp(&now, in, sizeof(now))) return;
    struct mix_settings *m = mix_edit();
    m->comp[c] = in->comp;
    m->reverb_send[c] = in->reverb_send;
    m->delay_send[c] = in->delay_send;
    m->send_pre[c] = in->send_pre;
    memcpy(m->send_route[c], in->send_route, sizeof(in->send_route));
    mix_fx_update();
}

struct mix_settings *mix_edit(void)
{
    struct mix_settings *live = (struct mix_settings *)preset.mixfx;
    if (live->format != MIX_FORMAT) { *live = mix_defaults; live->format = MIX_FORMAT; }
    return live;
}

const char *const mix_ratio_names[MIX_RATIOS] = {"Off", "1.5:1", "2:1", "3:1", "4:1", "6:1", "10:1", "20:1", "Limit"};
/* 1 - 1/ratio, Q16 */
static const u32 ratio_slope[MIX_RATIOS] = {0, 21845, 32768, 43691, 49152, 54613, 58982, 62259, 65536};
const u16 mix_attack_hundredths_ms[MIX_ATTACKS] = {5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000};
const u16 mix_release_ms[MIX_RELEASES] = {20, 50, 100, 200, 500, 1000, 2000};
const char *const mix_division_names[MIX_DIVISIONS] = {"1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/1"};
static const u8 division_units[MIX_DIVISIONS] = {12, 16, 18, 24, 32, 36, 48, 64, 72, 96, 192};

/* The DEP-5 reverb's network, in 32 kHz samples: the pre-delay, four
 * allpasses and eight damped combs, and the taps each output sums. */
#define PREDELAY        294u
#define ALLPASSES       4u
#define COMBS           8u
#define ALLPASS_GAIN    19661   /* 0.6 */
#define REVERB_IN_GAIN  9830    /* 0.3 */
#define COMB_IN_GAIN    7248    /* 0.2212 */
#define OUTPUT_GAIN     27313   /* 0.8335 * 2.0, applied as Q14 */
#define COMB_SHIFT      3       /* the combs run 18 dB hotter than the DEP-5's for 16-bit cells; their taps undo it */
#define EARLY_TAPS      8u
#define LATE_TAPS       8u
static const u16 allpass_len[ALLPASSES] = {1197, 715, 211, 53};
static const u16 comb_len[COMBS] = {1117, 1281, 1470, 1686, 1934, 2219, 2546, 2921};
/* A tap: a line (0-7 a comb, 8 the input tape), a delay into it, a gain (Q14). */
struct tap { u8 line; u16 delay; s16 gain; };
#define INPUT_TAPE 8u
static const struct tap taps[2][LATE_TAPS + EARLY_TAPS] = {
    { {2, 1470, 16673}, {4, 1934, 19040}, {6, 2546, 21696}, {4, 967, 19040}, {7, 1460, -3280},
      {0, 558, 14575}, {2, 735, 16673}, {0, 1117, 14575},
      {INPUT_TAPE, 358, 256}, {INPUT_TAPE, 785, 696}, {INPUT_TAPE, 1018, -344}, {INPUT_TAPE, 1262, -688},
      {INPUT_TAPE, 1557, 600}, {INPUT_TAPE, 2335, 655}, {INPUT_TAPE, 3252, 632} },
    { {3, 1686, 17791}, {5, 2219, 20319}, {7, 2921, -3280}, {5, 1109, 20319}, {6, 1273, 21696},
      {1, 640, 15584}, {3, 843, 17791}, {1, 1281, 15584},
      {INPUT_TAPE, 358, 256}, {INPUT_TAPE, 736, -705}, {INPUT_TAPE, 1018, -600}, {INPUT_TAPE, 1342, -680},
      {INPUT_TAPE, 1557, 311}, {INPUT_TAPE, 2097, -664}, {INPUT_TAPE, 3062, -641}, {INPUT_TAPE, 4223, -624} },
};
#define TAPE_LEN 4224u
#define LOG2_THOUSAND_Q24 167201000 /* 3 log2(10) = log2(1000), Q24 */
static const u8 tap_count[2] = {LATE_TAPS + EARLY_TAPS - 1, LATE_TAPS + EARLY_TAPS};

struct comp_state { s32 gr; };
struct comp_coef { u32 slope, attack, release; s32 threshold, makeup, mix_q15; u8 on, drive; };
/* Drive curves, (1 + k) v / (1 + k v) for v in 0..1 in 256 steps, Q16;
 * built in the foreground when a Drive changes. k doubles every 20 % of
 * Drive, up to 31 (+30 dB for small signals) at 100. */
#define DRIVE_STEPS        256u
#define DRIVE_OCTAVES_Q24  (5 << 24)
static u16 drive_curve[MIX_CHANNELS + 1][DRIVE_STEPS + 1];
static u8 drive_built[MIX_CHANNELS + 1];

static s16 reverb_ram[REVERB_WORDS];
static s16 delay_ram[2][DELAY_WORDS];
static u32 reverb_base;
static u16 line_start[INPUT_TAPE + 1]; /* each line's write offset */
static u16 allpass_start[ALLPASSES];
static s32 comb_gain[COMBS] __attribute__((aligned(16))), comb_damp[COMBS] __attribute__((aligned(16))),
    comb_lp[COMBS] __attribute__((aligned(16)));
/* The resamplers' histories, oldest first: the reverb send at 48 kHz, the
 * reverb's sides at 32 kHz (upstream: shift registers, newest first), with
 * room for a block. Their taps are kept reversed to match, and the reverb's
 * inputs and returns for a block. */
#define DOWN_KEEP (MIX_DOWN_TAPS - 1u)
#define BLOCK_TICKS (AUDIO_BLOCK * 2u / 3u + 1u)
static s16 down_line[DOWN_KEEP + AUDIO_BLOCK] __attribute__((aligned(16)));
static s16 up_line[2][MIX_UP_TAPS + BLOCK_TICKS] __attribute__((aligned(16)));
static s16 down_taps[2][MIX_DOWN_TAPS] __attribute__((aligned(16)));
static s16 up_taps[3][MIX_UP_TAPS + 4] __attribute__((aligned(16)));
static s16 tick_in[BLOCK_TICKS];
static s32 wet_block[AUDIO_BLOCK][2];
static u32 phase;
static s32 reverb_level_q15, delay_level_q15;
/* Sidechain sources each frame of the block, as MIX_KEY_* numbers: the
 * channels before their compressors, the input (Q31 full scale). */
static s32 keys[AUDIO_BLOCK][MIX_KEY_INPUT + 1];
static struct comp_coef coef[MIX_CHANNELS + 1];
static struct comp_state state[MIX_CHANNELS + 1];
static u32 delay_write, delay_q8[2], delay_target_q8;
static s32 feedback_q15, low_coef, high_coef, low_state[2], high_state[2];
static u32 frames;
static volatile u32 beat_q8;   /* the delay tempo's beat, Q8 frames, from the foreground; 0: none */

/* ---- log and exp ---- */

static inline s32 log2_q24(u32 x) /* x > 0 */
{
    u32 lead = 31u - (u32)__builtin_clz(x);
    u32 frac = lead >= 12 ? x >> (lead - 12) : x << (12 - lead);  /* 1.12 mantissa */
    u32 i = (frac >> 6) & 63u, f = frac & 63u;
    u32 l = mix_log2_table[i] + (((mix_log2_table[i + 1] - mix_log2_table[i]) * f) >> 6);
    return (s32)(lead << 24) + (s32)(l << 8);
}

/* 2^(x / 2^24) in Q24, x at most 6 octaves; 0 below 24 octaves down. */
static inline s32 exp2_q24(s32 x)
{
    if (x < LEVEL_FLOOR) return 0;
    s32 whole = x >> 24;
    u32 frac = (u32)x & 0xffffffu, i = frac >> 18, f = (frac >> 10) & 255u;
    u32 m = mix_exp2_table[i] + (u32)(((u64)(mix_exp2_table[i + 1] - mix_exp2_table[i]) * f) >> 8); /* Q30 */
    s32 shift = 6 - whole;  /* Q30 to Q24 */
    return shift >= 0 ? (s32)(m >> shift) : (s32)(m << -shift);
}

static inline s32 smooth(s32 at, s32 target, u32 coef_q31) { return at + (s32)(((s64)(target - at) * coef_q31) >> 31); }
static inline s32 sat32(s64 v) { return v > 0x7fffffff ? 0x7fffffff : v < -0x7fffffff ? -0x7fffffff : (s32)v; }
static inline s32 sat16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* ---- compressor ---- */

/* A one-pole step for a time constant in hundredths of a millisecond, Q31: 1 / (1 + frames). */
static u32 time_coef(u32 hundredths_ms) { return (u32)(((u64)0x80000000u * 100u) / (100u + (u64)hundredths_ms * (FRAME_HZ / 1000u))); }

static void comp_coef_set(struct comp_coef *k, const struct mix_comp_set *s)
{
    k->on = s->ratio != 0;
    k->slope = ratio_slope[s->ratio < MIX_RATIOS ? s->ratio : 0];
    k->threshold = -DB_TO_Q24(s->threshold);
    k->makeup = DB_TO_Q24(s->gain);
    k->mix_q15 = (s32)(s->mix * Q15_ONE / MIX_PERCENT_MAX);
    k->drive = s->drive;
    k->attack = time_coef(mix_attack_hundredths_ms[s->attack < MIX_ATTACKS ? s->attack : 0]);
    k->release = time_coef(100u * mix_release_ms[s->release < MIX_RELEASES ? s->release : 0]);
}

/* Drive: the curve, odd-symmetric, interpolated. A channel (shift
 * CHANNEL_SHIFT) above full scale reads the curve's end. */
static inline s32 drive(const u16 *curve, s32 x, u32 shift)
{
    u32 a = x < 0 ? (u32)-(x + 1) : (u32)x;
    if (shift) a = a >= 0x40000000u ? 0x7fffffffu : a << 1;
    u32 i = a >> 23, f = (a >> 15) & 255u;
    u32 y = curve[i] + (u32)(((s32)(curve[i + 1] - curve[i]) * (s32)f) >> 8);
    return x < 0 ? -(s32)(y << (15 - shift)) : (s32)(y << (15 - shift));
}

static void drive_build(u16 *curve, u32 amount)
{
    /* k = 2^(5 amount / 100) - 1, Q16 */
    u64 k = ((u64)(u32)exp2_q24((s32)((s64)DRIVE_OCTAVES_Q24 * amount / MIX_PERCENT_MAX)) >> 8) - 65536u;
    for (u32 i = 0; i <= DRIVE_STEPS; ++i) {
        u64 v = (u64)i << 8;                          /* Q16 */
        u64 y = ((65536u + k) * v) / (65536u + ((k * v) >> 16));
        curve[i] = (u16)(y > 65535u ? 65535u : y);
    }
}

/* Parallel compression: the dry signal and the compressed one by Mix. */
static inline s32 blend(s32 dry, s32 wet, s32 mix_q15) { return sat32((s64)dry + ((((s64)wet - dry) * mix_q15) >> 15)); }

static inline s32 apply(s32 x, s32 gain_q24) { return sat32(((s64)x * gain_q24) >> 24); }

/* ---- reverb ---- */

/* Q15 products truncated toward zero, so a tail cannot sustain itself at
 * the last bits as rounding lets it. */
static inline s32 mul15(s32 a, s32 b) { return (a * b) / 32768; }
static inline s16 cell(u32 offset) { return reverb_ram[(reverb_base + offset) & REVERB_MASK]; }
static inline void cell_set(u32 offset, s32 v) { reverb_ram[(reverb_base + offset) & REVERB_MASK] = (s16)sat16(v); }
/* A value written at `start` reads back at start + delay after `delay` ticks
 * (the base moves down one cell a tick). */
static inline s32 line_read(u32 start, u32 delay) { return cell(start + delay); }

/* The algorithms: the DEP-5's number, the network, wander, and the chorus's input. */
enum { NET_NONE, NET_HALL, NET_GATED, NET_SPARSE };
enum { CHORUS_NONE, CHORUS_SEND, CHORUS_REVERB, CHORUS_REVERB_ONLY };
struct algo { u8 net, wander, chorus; };
static const struct algo algos[MIX_ALGOS] = {
    {NET_NONE, 0, CHORUS_SEND},          /* 1 chorus */
    {NET_HALL, 0, CHORUS_NONE},          /* 2 reverb */
    {NET_HALL, 1, CHORUS_NONE},          /* 3 reverb mod */
    {NET_HALL, 0, CHORUS_REVERB_ONLY},   /* 4 chorus to reverb: the chorused reverb only */
    {NET_HALL, 0, CHORUS_SEND},          /* 5 reverb, chorus parallel */
    {NET_HALL, 0, CHORUS_REVERB},        /* 6 reverb, chorus series/parallel */
    {NET_GATED, 0, CHORUS_NONE},         /* 7 nonlinear */
    {NET_GATED, 1, CHORUS_NONE},         /* 8 nonlinear mod */
    {NET_GATED, 0, CHORUS_REVERB},       /* 9 nonlinear chorus */
    {NET_SPARSE, 1, CHORUS_NONE},        /* 11 low density nonlinear mod */
};
const char *const mix_algo_names[MIX_ALGOS] = {"1 Chorus", "2 Reverb", "3 Rev Mod", "4 Cho Rev", "5 Rev+Cho",
    "6 Ser/Par", "7 NonLin", "8 NL Mod", "9 NL Chorus", "11 LD NL Mod"};
u32 mix_algo_chorus(u32 algo) { return algo < MIX_ALGOS && algos[algo].chorus != CHORUS_NONE; }

/* The DEP-5's nonlinear network: a diffuser (four allpasses; one for low
 * density) into a tape, read by ten taps a side out to 125 ms at Gate 125. */
#define GATED_ALLPASSES   4u
#define GATED_TAPS        10u
#define GATED_TAPE_LEN    16384u   /* taps reach 4000 x 4 at the 500 ms gate */
#define GATED_IN_GAIN     9830     /* 0.3 */
#define GATED_TAPE_GAIN   6144     /* 0.375 x 0.5 */
#define SPARSE_TAPE_GAIN  12288    /* 0.375 */
#define GATE_MS_PER_TENTH 5u       /* Decay 2.5 s reads as a 125 ms gate */
#define GATE_BASE_MS      125u
static const u16 gated_allpass_len[GATED_ALLPASSES] = {263, 159, 89, 69};
static const s16 gated_allpass_gain[GATED_ALLPASSES] = {14896, 16384, 16384, 16384};
#define SPARSE_ALLPASS_GAIN 14031  /* 0.4282 */
static const u16 gated_delay[2][GATED_TAPS] = {
    {333, 622, 922, 1222, 1558, 1927, 2297, 2722, 3187, 3707},
    {483, 760, 1061, 1396, 1742, 2112, 2517, 2956, 3436, 4000},
};
static const s16 gated_gain[2][GATED_TAPS] = {   /* Q12: 2.0 falling to 1.78 */
    {8192, 8088, 7992, 7888, 7792, 7696, 7600, 7504, 7408, 7320},
    {8144, 8040, 7936, 7840, 7744, 7648, 7552, 7456, 7360, 7272},
};

/* Chorus: two reads of one line sweep against each other by a triangle,
 * left longer while right is shorter, around 2.8 ms (the DEP-5 board's
 * MB654119 setting). */
#define CHORUS_WORDS      1024u
#define CHORUS_MASK       (CHORUS_WORDS - 1u)
#define CHORUS_BASE_Q8    (90u << 8)
#define CHORUS_DEPTH_MAX  160u     /* samples of sweep at Depth 100: 5 ms */
#define CHORUS_RATE_LOW   10737u   /* 0.08 Hz as a 32-bit phase step at 32 kHz */
#define CHORUS_RATE_OCTAVES 100147000 /* 5.97 octaves, Q24: Rate 100 is 5 Hz */
static s16 chorus_ram[CHORUS_WORDS];
static u32 chorus_write, chorus_phase, chorus_step, chorus_depth_q8;

/* Wander for the Mod algorithms: the DEP-5 stretches its frame with
 * filtered noise. Here every long read moves by the same small fraction,
 * which drifts toward a new random target every 4096 ticks. */
#define WANDER_PERIOD     4096u
#define WANDER_RANGE      164      /* +-0.25 %, Q16 */
#define WANDER_SHIFT      11
#define WANDER_ROOM       16u      /* cells: 0.25 % of the longest comb, and the interpolation's next */
static s32 wander, wander_target;
static u32 wander_seed = 0x2545f491u, wander_count;

static u32 algo_now = MIX_ALGOS, clearing;
static u32 gated_scale_q8 = 256u;
static u16 gated_allpass_start[GATED_ALLPASSES], gated_tape_start;
static s32 gated_lp;

#define CLEAR_WORDS       256u     /* a changed algorithm clears its memory this much a tick (4 ms in all) */

/* Each tap's cell offset (its line's start plus its delay), and each comb's
 * and allpass's read offset, from reverb_layout. */
static u16 tap_at[2][LATE_TAPS + EARLY_TAPS], comb_at[COMBS], allpass_at[ALLPASSES];
static s16 tap_gain[2][LATE_TAPS + EARLY_TAPS] __attribute__((aligned(16)));   /* 0 past a side's last tap */

static void reverb_layout(void)
{
    u32 at = 0;
    line_start[INPUT_TAPE] = (u16)at; at += TAPE_LEN + 1;
    for (u32 i = 0; i < ALLPASSES; ++i) { allpass_start[i] = (u16)at; at += allpass_len[i] + 1u; }
    /* Each comb has room past its length for the wander's longest read. */
    for (u32 i = 0; i < COMBS; ++i) { line_start[i] = (u16)at; at += comb_len[i] + 1u + WANDER_ROOM; }
    /* The gated network shares the memory: an algorithm change clears it. */
    at = 0;
    for (u32 i = 0; i < GATED_ALLPASSES; ++i) { gated_allpass_start[i] = (u16)at; at += gated_allpass_len[i] + 1u; }
    gated_tape_start = (u16)at;
    for (u32 side = 0; side < 2; ++side)
        for (u32 t = 0; t < tap_count[side]; ++t) {
            tap_at[side][t] = (u16)(line_start[taps[side][t].line] + taps[side][t].delay);
            tap_gain[side][t] = taps[side][t].gain;
        }
    for (u32 i = 0; i < COMBS; ++i) comb_at[i] = (u16)(line_start[i] + comb_len[i]);
    for (u32 i = 0; i < ALLPASSES; ++i) allpass_at[i] = (u16)(allpass_start[i] + allpass_len[i]);
}

/* A read `delay` back, moved by the wander (Q16 of the delay), interpolated. */
static inline s32 line_read_wander(u32 start, u32 delay)
{
    s32 q8 = (s32)(delay << 8) + (s32)((s32)delay * wander >> 8);
    u32 whole = (u32)q8 >> 8, f = (u32)q8 & 255u;
    s32 a = cell(start + whole), b = cell(start + whole + 1u);
    return a + (((b - a) * (s32)f) >> 8);
}

__attribute__((optimize("no-tree-vectorize"))) static void hall_tick(s32 in, s32 *left, s32 *right, u32 wandering)
{
    cell_set(line_start[INPUT_TAPE], in);
    s32 x = mul15(line_read(line_start[INPUT_TAPE], PREDELAY), REVERB_IN_GAIN);
    for (u32 i = 0; i < ALLPASSES; ++i) {
        s32 d = cell(allpass_at[i]);
        s32 w = sat16(x - mul15(d, ALLPASS_GAIN));
        cell_set(allpass_start[i], w);
        x = d + mul15(w, ALLPASS_GAIN);
    }
    x = mul15(x, COMB_IN_GAIN) * (1 << COMB_SHIFT);
    for (u32 i = 0; i < COMBS; ++i) {
        s32 d = wandering ? line_read_wander(line_start[i], comb_len[i]) : cell(comb_at[i]);
        comb_lp[i] = d + mul15(comb_lp[i] - d, comb_damp[i]);
        cell_set(line_start[i], x + mul15(comb_lp[i], comb_gain[i]));
    }
    const s16 *ram = reverb_ram;
    const u32 base = reverb_base;
    for (u32 side = 0; side < 2; ++side) {
        const u16 *at = tap_at[side];
        s64 late = 0, early = 0;
#pragma GCC unroll 8
        for (u32 t = 0; t < LATE_TAPS; ++t)
            late += ram[(base + at[t]) & REVERB_MASK] * taps[side][t].gain;
        if (side) {
#pragma GCC unroll 8
            for (u32 t = LATE_TAPS; t < LATE_TAPS + EARLY_TAPS; ++t)
                early += ram[(base + at[t]) & REVERB_MASK] * taps[1][t].gain;
        } else {
#pragma GCC unroll 8
            for (u32 t = LATE_TAPS; t < LATE_TAPS + EARLY_TAPS - 1u; ++t)
                early += ram[(base + at[t]) & REVERB_MASK] * taps[0][t].gain;
        }
        s32 out = (s32)(((late >> COMB_SHIFT) + early) >> 14);
        *(side ? right : left) = sat16((out * OUTPUT_GAIN) >> 14);
    }
}

/* Q15 products truncated toward zero, four at a time (mul15). */
static inline int32x4_t mul15_4(int32x4_t a, int32x4_t b)
{
    int32x4_t p = vmulq_s32(a, b);
    uint32x4_t round = vshrq_n_u32(vreinterpretq_u32_s32(vshrq_n_s32(p, 31)), 17);
    return vshrq_n_s32(vaddq_s32(p, vreinterpretq_s32_u32(round)), 15);
}

/* hall_tick without wander: the combs and the taps four or eight at a time
 * (the combs' cells are all read before any is written, as no comb reads
 * another's; no tap reads a cell written this tick). Same results. */
static void hall_tick_lanes(s32 in, s32 *left, s32 *right)
{
    s16 *ram = reverb_ram;
    const u32 base = reverb_base;
#define AT(offset) (&ram[(base + (offset)) & REVERB_MASK])
    *AT(line_start[INPUT_TAPE]) = (s16)sat16(in);
    s32 x = mul15(*AT(line_start[INPUT_TAPE] + PREDELAY), REVERB_IN_GAIN);
    for (u32 i = 0; i < ALLPASSES; ++i) {
        s32 d = *AT(allpass_at[i]);
        s32 w = sat16(x - mul15(d, ALLPASS_GAIN));
        *AT(allpass_start[i]) = (s16)w;
        x = d + mul15(w, ALLPASS_GAIN);
    }
    x = mul15(x, COMB_IN_GAIN) * (1 << COMB_SHIFT);
    int16x8_t d = vdupq_n_s16(0);
    d = vld1q_lane_s16(AT(comb_at[0]), d, 0);
    d = vld1q_lane_s16(AT(comb_at[1]), d, 1);
    d = vld1q_lane_s16(AT(comb_at[2]), d, 2);
    d = vld1q_lane_s16(AT(comb_at[3]), d, 3);
    d = vld1q_lane_s16(AT(comb_at[4]), d, 4);
    d = vld1q_lane_s16(AT(comb_at[5]), d, 5);
    d = vld1q_lane_s16(AT(comb_at[6]), d, 6);
    d = vld1q_lane_s16(AT(comb_at[7]), d, 7);
    int32x4_t d0 = vmovl_s16(vget_low_s16(d)), d1 = vmovl_s16(vget_high_s16(d));
    int32x4_t lp0 = vaddq_s32(d0, mul15_4(vsubq_s32(vld1q_s32(comb_lp), d0), vld1q_s32(comb_damp)));
    int32x4_t lp1 = vaddq_s32(d1, mul15_4(vsubq_s32(vld1q_s32(comb_lp + 4), d1), vld1q_s32(comb_damp + 4)));
    vst1q_s32(comb_lp, lp0);
    vst1q_s32(comb_lp + 4, lp1);
    const int32x4_t xv = vdupq_n_s32(x);
    int16x8_t w = vcombine_s16(vqmovn_s32(vaddq_s32(xv, mul15_4(lp0, vld1q_s32(comb_gain)))),
                               vqmovn_s32(vaddq_s32(xv, mul15_4(lp1, vld1q_s32(comb_gain + 4)))));
    vst1q_lane_s16(AT(line_start[0]), w, 0);
    vst1q_lane_s16(AT(line_start[1]), w, 1);
    vst1q_lane_s16(AT(line_start[2]), w, 2);
    vst1q_lane_s16(AT(line_start[3]), w, 3);
    vst1q_lane_s16(AT(line_start[4]), w, 4);
    vst1q_lane_s16(AT(line_start[5]), w, 5);
    vst1q_lane_s16(AT(line_start[6]), w, 6);
    vst1q_lane_s16(AT(line_start[7]), w, 7);
    int64x2_t lsum[2], esum[2];
    for (u32 side = 0; side < 2; ++side) {
        const u16 *at = tap_at[side];
        int16x8_t a = vdupq_n_s16(0), b = a;
        a = vld1q_lane_s16(AT(at[0]), a, 0);
        a = vld1q_lane_s16(AT(at[1]), a, 1);
        a = vld1q_lane_s16(AT(at[2]), a, 2);
        a = vld1q_lane_s16(AT(at[3]), a, 3);
        a = vld1q_lane_s16(AT(at[4]), a, 4);
        a = vld1q_lane_s16(AT(at[5]), a, 5);
        a = vld1q_lane_s16(AT(at[6]), a, 6);
        a = vld1q_lane_s16(AT(at[7]), a, 7);
        b = vld1q_lane_s16(AT(at[8]), b, 0);
        b = vld1q_lane_s16(AT(at[9]), b, 1);
        b = vld1q_lane_s16(AT(at[10]), b, 2);
        b = vld1q_lane_s16(AT(at[11]), b, 3);
        b = vld1q_lane_s16(AT(at[12]), b, 4);
        b = vld1q_lane_s16(AT(at[13]), b, 5);
        b = vld1q_lane_s16(AT(at[14]), b, 6);
        b = vld1q_lane_s16(AT(at[15]), b, 7);
        int16x8_t ga = vld1q_s16(tap_gain[side]), gb = vld1q_s16(tap_gain[side] + 8);
        lsum[side] = vaddq_s64(vpaddlq_s32(vmull_s16(vget_low_s16(a), vget_low_s16(ga))),
                               vpaddlq_s32(vmull_s16(vget_high_s16(a), vget_high_s16(ga))));
        esum[side] = vaddq_s64(vpaddlq_s32(vmull_s16(vget_low_s16(b), vget_low_s16(gb))),
                               vpaddlq_s32(vmull_s16(vget_high_s16(b), vget_high_s16(gb))));
    }
    /* Each side's sums, left and right in their lanes. */
    int64x2_t late = vaddq_s64(vcombine_s64(vget_low_s64(lsum[0]), vget_low_s64(lsum[1])),
                               vcombine_s64(vget_high_s64(lsum[0]), vget_high_s64(lsum[1])));
    int64x2_t early = vaddq_s64(vcombine_s64(vget_low_s64(esum[0]), vget_low_s64(esum[1])),
                                vcombine_s64(vget_high_s64(esum[0]), vget_high_s64(esum[1])));
#undef AT
    int32x2_t out = vmovn_s64(vshrq_n_s64(vaddq_s64(vshrq_n_s64(late, COMB_SHIFT), early), 14));
    int16x4_t o = vqmovn_s32(vcombine_s32(vshr_n_s32(vmul_n_s32(out, OUTPUT_GAIN), 14), out));
    *left = vget_lane_s16(o, 0);
    *right = vget_lane_s16(o, 1);
}

static void gated_tick(s32 in, s32 *left, s32 *right, u32 sparse, u32 wandering)
{
    /* Damp: a low-pass ahead of the diffuser. */
    s32 x = mul15(in, GATED_IN_GAIN);
    gated_lp = x + mul15(gated_lp - x, comb_damp[COMBS - 1]);
    x = gated_lp;
    u32 passes = sparse ? 1u : GATED_ALLPASSES;
    for (u32 i = 0; i < passes; ++i) {
        s32 g = sparse ? SPARSE_ALLPASS_GAIN : gated_allpass_gain[i];
        s32 d = line_read(gated_allpass_start[i], gated_allpass_len[i]);
        s32 w = sat16(x - mul15(d, g));
        cell_set(gated_allpass_start[i], w);
        x = d + mul15(w, g);
    }
    cell_set(gated_tape_start, mul15(x, sparse ? SPARSE_TAPE_GAIN : GATED_TAPE_GAIN) * (1 << COMB_SHIFT));
    for (u32 side = 0; side < 2; ++side) {
        s64 sum = 0;
        for (u32 t = 0; t < GATED_TAPS; ++t) {
            u32 delay = (gated_delay[side][t] * gated_scale_q8) >> 8;
            s32 v = wandering ? line_read_wander(gated_tape_start, delay) : line_read(gated_tape_start, delay);
            sum += v * gated_gain[side][t];
        }
        *(side ? right : left) = sat16((s32)((((sum >> 12) >> COMB_SHIFT) * OUTPUT_GAIN) >> 14));
    }
}

static inline s32 chorus_read(u32 q8)
{
    u32 whole = q8 >> 8, f = q8 & 255u;
    s32 a = chorus_ram[(chorus_write - whole) & CHORUS_MASK], b = chorus_ram[(chorus_write - whole - 1u) & CHORUS_MASK];
    return a + (((b - a) * (s32)f) >> 8);
}

/* One 32 kHz tick of the selected algorithm: Q15 in, Q15 out. */
static void reverb_tick(s32 in, s32 *left, s32 *right)
{
    u32 a = mix_now()->reverb_algo < MIX_ALGOS ? mix_now()->reverb_algo : 1u;
    if (a != algo_now) { algo_now = a; clearing = REVERB_WORDS / CLEAR_WORDS; }
    if (clearing) {
        /* Silent while the memory clears, a slice a tick. */
        --clearing;
        int16x8_t z = vdupq_n_s16(0);
        for (s16 *p = &reverb_ram[clearing * CLEAR_WORDS], *e = p + CLEAR_WORDS; p < e; p += 32) {
            vst1q_s16(p, z); vst1q_s16(p + 8, z); vst1q_s16(p + 16, z); vst1q_s16(p + 24, z);
        }
        for (u32 i = 0; i < COMBS; ++i) comb_lp[i] = 0;
        gated_lp = 0;
        *left = *right = 0;
        return;
    }
    const struct algo *alg = &algos[a];
    if (alg->wander) {
        if (++wander_count >= WANDER_PERIOD) {
            wander_count = 0;
            wander_seed = wander_seed * 1664525u + 1013904223u;
            wander_target = (s32)(wander_seed >> 16) % (2 * WANDER_RANGE + 1) - WANDER_RANGE;
        }
        wander += (wander_target - wander) >> WANDER_SHIFT;
    }
    s32 l = 0, r = 0;
    if (alg->net == NET_HALL) {
        if (alg->wander) hall_tick(in, &l, &r, 1);
        else hall_tick_lanes(in, &l, &r);
    }
    else if (alg->net != NET_NONE) gated_tick(in, &l, &r, alg->net == NET_SPARSE, alg->wander);
    reverb_base = (reverb_base - 1u) & REVERB_MASK;
    if (alg->chorus != CHORUS_NONE) {
        chorus_ram[chorus_write] = (s16)(alg->chorus == CHORUS_SEND ? in : sat16((l + r) >> 1));
        chorus_phase += chorus_step;
        u32 tri = chorus_phase < 0x80000000u ? chorus_phase >> 15 : (~chorus_phase) >> 15;  /* 0..65535 */
        u32 sweep_l = (u32)(((u64)chorus_depth_q8 * tri) >> 16), sweep_r = chorus_depth_q8 - sweep_l;
        s32 cl = chorus_read(CHORUS_BASE_Q8 + sweep_l), cr = chorus_read(CHORUS_BASE_Q8 + sweep_r);
        chorus_write = (chorus_write + 1u) & CHORUS_MASK;
        if (alg->chorus == CHORUS_REVERB_ONLY) { l = 0; r = 0; }
        l = sat16(l + cl); r = sat16(r + cr);
    }
    *left = l; *right = r;
}

/* Comb gains for the decay time (10^(-3 L / (T fs)), as the DEP-5) and
 * damping in proportion to each comb's length (the DEP-5's at Damp 20). */
static u32 comb_decay_k[COMBS], comb_damp_k[COMBS];
static void reverb_constants(void)
{
    for (u32 i = 0; i < COMBS; ++i) {
        comb_decay_k[i] = (u32)((u64)LOG2_THOUSAND_Q24 * comb_len[i] * 10u / REVERB_HZ); /* x tenths of a second */
        comb_damp_k[i] = comb_len[i] * 429u / 400u;
    }
}
static void reverb_update(u32 tenths, u32 damp)
{
    for (u32 i = 0; i < COMBS; ++i) {
        /* log2 of the gain: -3 log2(10) L / (T fs), Q24 */
        s32 g = exp2_q24(-(s32)(comb_decay_k[i] / tenths)); /* Q24, below one */
        comb_gain[i] = -(g >> 9);   /* Q15, negative as the DEP-5's */
        /* a = L * 1.31e-5 * (damp / 20)^2, Q15, at most 0.9 */
        u32 a = comb_damp_k[i] * damp * damp / 1000u;
        comb_damp[i] = (s32)(a > 29491u ? 29491u : a);
    }
}

/* ---- delay ---- */

static inline s32 delay_read(u32 side)
{
    u32 q8 = delay_q8[side], back = q8 >> 8, f = q8 & 255u;
    s32 a = delay_ram[side][(delay_write - back) & DELAY_MASK];
    s32 b = delay_ram[side][(delay_write - back - 1u) & DELAY_MASK];
    return a + (((b - a) * (s32)f) >> 8);
}

static inline s32 soft_clip(s32 v) /* Q15 in, Q15 out within +-1 */
{
    if (v > SOFT_CLIP_LIMIT) v = SOFT_CLIP_LIMIT;
    if (v < -SOFT_CLIP_LIMIT) v = -SOFT_CLIP_LIMIT;
    /* v - v^3 / 6.75 (Q45 cube): upstream's cube * 4 / (27 << 30) toward
     * zero, as two exact steps (no 64-bit division). */
    u64 m = (u64)((s64)(v < 0 ? -v : v) * v * v);
    s32 q = (s32)((u32)((m >> 28) & 0xffffffffu) / 27u);
    return v < 0 ? v + q : v - q;
}

/* The delay's beat in Q8 frames from its Tempo: MIDI clock or the CV
 * CLOCK through Time Lock's clock code, else the set BPM. fx.c marks the settings
 * changed. */
static void drives_foreground(void);
void mix_fx_foreground(void)
{
    drives_foreground();
    u32 tempo = mix_now()->delay_tempo;
    beat_q8 = tempo == MIX_TEMPO_CV ? ta_clock_beat_q8() : tempo == MIX_TEMPO_MIDI ? ta_midi_beat_q8() : 0;
}
/* Drive curves for the Drive settings, built here and used by the
 * interrupt once marked built (local: the mix's only; the channels work
 * theirs out, ta_mix_channels). */
static void drives_foreground(void)
{
    for (u32 c = MIX_CHANNELS; c <= MIX_CHANNELS; ++c) {
        u32 amount = c < MIX_CHANNELS ? mix_now()->comp[c].drive : mix_now()->master.drive;
        if (!amount || drive_built[c] == amount + 1u) continue;
        drive_built[c] = 0;
        MEMORY_BARRIER();
        drive_build(drive_curve[c], amount);
        MEMORY_BARRIER();
        drive_built[c] = (u8)(amount + 1u);
    }
}

u32 mix_fx_reduction_db(u32 compressor)
{
    s32 gr = state[compressor <= MIX_CHANNELS ? compressor : MIX_CHANNELS].gr;
    return gr > 0 ? (u32)(((s64)gr * 60206 / 10000 + (1 << 23)) >> 24) : 0;
}

static u32 set_bpm(void)
{
    return mix_tempo_bpm(mix_now()->delay_tempo);
}

u32 mix_fx_bpm10(void)
{
    u32 beat = beat_q8, bpm = set_bpm();
    if (bpm) return bpm * 10u;
    return beat ? (u32)((u64)FRAME_HZ * 600u * 256u / beat) : 0;
}

static u32 division_now;  /* the delay's Time after its CV */
static void delay_target(void)
{
    /* A stopped clock keeps the last time; no time yet is 120 BPM. */
    u32 bpm = set_bpm(), beat = bpm ? FRAME_HZ * 60u / bpm : beat_q8 >> 8;
    if (!beat) { if (delay_target_q8) return; beat = FRAME_HZ * 60u / 120u; }
    u32 d = division_now < MIX_DIVISIONS ? division_now : 0;
    u32 frames_d = beat * division_units[d] / DIVISION_UNITS;
    while (frames_d > DELAY_WORDS - 4u) frames_d >>= 1;
    if (frames_d < DELAY_MIN_FRAMES) frames_d = DELAY_MIN_FRAMES;
    delay_target_q8 = frames_d << 8;
}

/* ---- settings and CV ---- */

#define CONTROL_MASK       31u    /* the routed mix settings follow CV every 32 frames */
#define GLIDE_SHIFT        6      /* gains follow a change over about 1.3 ms */
#define CV_FULL_SCALE      32768  /* 5 V */
#define AMOUNT_FULL        100
static volatile u32 dirty;
static u32 comps_pending;          /* compressors whose coefficients are due, one a frame */
static s32 send_input[MIX_CHANNELS][MIX_SEND_ROUTES], bus_input[MIX_BUS_ROUTES]; /* CV inputs, -1: none */
static s32 send_now[MIX_CHANNELS][MIX_SEND_ROUTES];  /* Q15, gliding */
static s32 reverb_level_target, delay_level_target, feedback_target;
static u32 decay_now, damp_now;

void mix_fx_update(void) { dirty = 1; }

static s32 clamp32(s32 v, s32 low, s32 high) { return v < low ? low : v > high ? high : v; }
/* A setting moved by its route: at +100, 5 V crosses the whole range. */
static s32 routed(u32 base, const struct mix_route *r, s32 input, s32 low, s32 high)
{
    s32 v = (s32)base;
    if (input >= 0 && r->amount)
        v += r->amount * ((volatile s16 *)ta_cv_counts)[input] / AMOUNT_FULL * (high - low) / CV_FULL_SCALE;
    return clamp32(v, low, high);
}
static inline s32 glide(s32 at, s32 target) { return at + ((target - at) >> GLIDE_SHIFT); }

/* The routes' inputs and the compressors, after an edit. */
static void settings_changed(void)
{
    for (u32 c = 0; c < MIX_CHANNELS; ++c)
        for (u32 n = 0; n < MIX_SEND_ROUTES; ++n)
            send_input[c][n] = mix_now()->send_route[c][n].amount ? eq_cv_input(mix_now()->send_route[c][n].source, c) : -1;
    for (u32 n = 0; n < MIX_BUS_ROUTES; ++n)
        bus_input[n] = mix_now()->bus_route[n].amount ? eq_cv_input(mix_now()->bus_route[n].source, 0) : -1;
    comps_pending = (1u << (MIX_CHANNELS + 1)) - 1u;
}

/* The mix settings as their routes move them now. */
static u8 pair_right[MIX_CHANNELS];   /* a channel in Stereo/Right mode */
static u32 lanes_stale = 1;           /* ta_mix_channels' lane settings */
static void control(void)
{
    const struct mix_settings *m = mix_now();
    for (u32 c = 1; c < MIX_CHANNELS; ++c) pair_right[c] = (u8)ta_pair_right(c);
    lanes_stale = 1;
    const struct mix_route *r = m->bus_route;
    u32 decay = (u32)routed(m->reverb_decay, &r[MIX_ROUTE_REVERB_DECAY], bus_input[MIX_ROUTE_REVERB_DECAY], MIX_DECAY_MIN, MIX_DECAY_MAX);
    u32 damp = (u32)routed(m->reverb_damp, &r[MIX_ROUTE_REVERB_DAMP], bus_input[MIX_ROUTE_REVERB_DAMP], 0, MIX_PERCENT_MAX);
    if (decay != decay_now || damp != damp_now) { decay_now = decay; damp_now = damp; reverb_update(decay, damp); }
    reverb_level_target = routed(m->reverb_level, &r[MIX_ROUTE_REVERB_LEVEL], bus_input[MIX_ROUTE_REVERB_LEVEL], 0, MIX_PERCENT_MAX) * PERCENT_Q15;
    delay_level_target = routed(m->delay_level, &r[MIX_ROUTE_DELAY_LEVEL], bus_input[MIX_ROUTE_DELAY_LEVEL], 0, MIX_PERCENT_MAX) * PERCENT_Q15;
    feedback_target = routed(m->delay_feedback, &r[MIX_ROUTE_DELAY_FEEDBACK], bus_input[MIX_ROUTE_DELAY_FEEDBACK], 0, MIX_FEEDBACK_MAX) * PERCENT_Q15;
    u32 low = (u32)routed(m->delay_low, &r[MIX_ROUTE_DELAY_LOW], bus_input[MIX_ROUTE_DELAY_LOW], 0, MIX_FILTER_STEPS - 1);
    u32 high = (u32)routed(m->delay_high, &r[MIX_ROUTE_DELAY_HIGH], bus_input[MIX_ROUTE_DELAY_HIGH], 0, MIX_FILTER_STEPS - 1);
    low_coef = mix_low_cut_coef[low];
    high_coef = mix_high_cut_coef[high];
    u32 rate = (u32)routed(m->chorus_rate, &r[MIX_ROUTE_CHORUS_RATE], bus_input[MIX_ROUTE_CHORUS_RATE], 0, MIX_PERCENT_MAX);
    u32 depth = (u32)routed(m->chorus_depth, &r[MIX_ROUTE_CHORUS_DEPTH], bus_input[MIX_ROUTE_CHORUS_DEPTH], 0, MIX_PERCENT_MAX);
    chorus_step = (u32)(((u64)CHORUS_RATE_LOW * (u32)exp2_q24((s32)((s64)CHORUS_RATE_OCTAVES * rate / MIX_PERCENT_MAX))) >> 24);
    chorus_depth_q8 = depth * CHORUS_DEPTH_MAX * 256u / MIX_PERCENT_MAX;
    gated_scale_q8 = decay * GATE_MS_PER_TENTH * 256u / GATE_BASE_MS;
    u32 division = (u32)routed(m->delay_division, &r[MIX_ROUTE_DELAY_TIME], bus_input[MIX_ROUTE_DELAY_TIME], 0, MIX_DIVISIONS - 1);
    if (division != division_now) { division_now = division; delay_target(); }
}

/* ---- the block entries (local: upstream's per-frame hooks) ---- */

static inline s32 magnitude(s32 x) { return x < 0 ? -(x + 1) : x; }
/* A channel sample at Q31 full scale, for the detectors and sidechains. */
static inline s32 key_level(s32 x) { return sat32((s64)x * (1 << CHANNEL_SHIFT)); }

/* The channels' drives and compressors in float, four channels a vector
 * (upstream: integer, channel by channel, with table log, exp and drive
 * curves). The same model: drive (1 + k) v / (1 + k v) worked out, not
 * read from the curve; the detector's level in octaves; a hard knee; the
 * gain reduction's one-pole attack and release per frame; the gain
 * 2^(makeup - reduction); the dry and wet blended by Mix. emu/mixresp.py
 * holds it to the model. A Stereo/Right pair's right lane takes its
 * master's settings and gain; the master's detector reads the larger side. */
static float comp_gr[MIX_CHANNELS];

/* All ones where v's sign bit is set (GCC scalarises float vector compares). */
static inline uint32x4_t negative(float32x4_t v) { return vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_f32(v), 31)); }

/* log2 of x > 0: the exponent, and a fit of log2(1 + m) on [0, 1) (within
 * 4.5e-5 of an octave). */
static inline float32x4_t log2_f(float32x4_t x)
{
    int32x4_t b = vreinterpretq_s32_f32(x);
    float32x4_t e = vcvtq_f32_s32(vsubq_s32(vshrq_n_s32(b, 23), vdupq_n_s32(127)));
    float32x4_t m = vsubq_f32(vreinterpretq_f32_s32(vorrq_s32(vandq_s32(b, vdupq_n_s32(0x7fffff)),
                                                              vdupq_n_s32(0x3f800000))), vdupq_n_f32(1.0f));
    float32x4_t p = vmlaq_f32(vdupq_n_f32(-0.18772264f), m, vdupq_n_f32(0.043428909f));
    p = vmlaq_f32(vdupq_n_f32(0.40872175f), m, p);
    p = vmlaq_f32(vdupq_n_f32(-0.70570415f), m, p);
    p = vmlaq_f32(vdupq_n_f32(1.4412674f), m, p);
    return vmlaq_f32(e, m, p);
}

/* 2^x for x within -24..6 octaves, 0 below (as exp2_q24): the whole
 * octaves into the exponent, a fit of 2^f on [0, 1) (within 8e-6). */
static inline float32x4_t exp2_f(float32x4_t x)
{
    float32x4_t t = vaddq_f32(x, vdupq_n_f32(64.0f));
    int32x4_t n = vcvtq_s32_f32(t);
    float32x4_t f = vsubq_f32(t, vcvtq_f32_s32(n));
    float32x4_t p = vmlaq_f32(vdupq_n_f32(0.051666878f), f, vdupq_n_f32(0.013676531f));
    p = vmlaq_f32(vdupq_n_f32(0.24171026f), f, p);
    p = vmlaq_f32(vdupq_n_f32(0.69293129f), f, p);
    p = vmlaq_f32(vdupq_n_f32(1.0000073f), f, p);
    float32x4_t two_n = vreinterpretq_f32_s32(vshlq_n_s32(vaddq_s32(n, vdupq_n_s32(127 - 64)), 23));
    uint32x4_t below = negative(vsubq_f32(x, vdupq_n_f32((float)LEVEL_FLOOR / Q24_OCTAVE)));
    return vreinterpretq_f32_u32(vbicq_u32(vreinterpretq_u32_f32(vmulq_f32(p, two_n)), below));
}

/* Drive as upstream reads its curve: (1 + k) v / (1 + k v) at 256 steps of
 * v = |x| / 2^30 (up to 1), straight between them, odd, back at 2^30. With
 * u the step below v and h a step, that line is
 * (1 + k) (v + k u (u + h)) / ((1 + k u) (1 + k (u + h))). */
static inline float32x4_t drive_f(float32x4_t x, float32x4_t k)
{
    const float32x4_t one = vdupq_n_f32(1.0f), h = vdupq_n_f32(1.0f / DRIVE_STEPS);
    float32x4_t v = vminq_f32(vmulq_f32(vabsq_f32(x), vdupq_n_f32(1.0f / 1073741824.0f)), one);
    uint32x4_t i = vminq_u32(vcvtq_u32_f32(vmulq_f32(v, vdupq_n_f32((float)DRIVE_STEPS))), vdupq_n_u32(DRIVE_STEPS - 1u));
    float32x4_t u = vmulq_f32(vcvtq_f32_u32(i), h), uh = vaddq_f32(u, h);
    float32x4_t d = vmulq_f32(vmlaq_f32(one, k, u), vmlaq_f32(one, k, uh));
    float32x4_t r = vrecpeq_f32(d);
    r = vmulq_f32(r, vrecpsq_f32(d, r));
    r = vmulq_f32(r, vrecpsq_f32(d, r));
    float32x4_t y = vmulq_f32(vmulq_f32(vaddq_f32(k, one), vmlaq_f32(v, vmulq_f32(k, u), uh)), r);
    y = vmulq_f32(vminq_f32(y, one), vdupq_n_f32(1073741824.0f));
    /* x's sign onto y (y >= 0). */
    return vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(y), vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u))));
}

static inline float32x4_t lanes_f(const float *v) { return vld1q_f32(v); }
static inline uint32x4_t lanes_u(const u32 *v) { return vld1q_u32(v); }

void ta_mix_channels(s32 (*x)[MIX_CHANNELS], const s32 (*in)[2], u32 count)
{
    const int32x4_t floor = vdupq_n_s32(-0x7fffffff);
    for (u32 f = 0; f < count; ++f) {
        s32 *key = keys[f];
        /* key_level: doubled, held within +-(2^31 - 1). */
        vst1q_s32(key + 1, vmaxq_s32(vqshlq_n_s32(vld1q_s32(x[f]), CHANNEL_SHIFT), floor));
        vst1q_s32(key + 5, vmaxq_s32(vqshlq_n_s32(vld1q_s32(x[f] + 4), CHANNEL_SHIFT), floor));
        /* The Sample input as a sidechain: the larger side. */
        s32 l = magnitude(in[f][0]), r = magnitude(in[f][1]);
        key[MIX_KEY_INPUT] = l > r ? l : r;
    }

    /* Each lane's settings: its own, or a right side's master's (local:
     * worked out again only after the coefficients or pairs change). */
    static float k[8] __attribute__((aligned(16))), thr[8] __attribute__((aligned(16))), slope[8] __attribute__((aligned(16)));
    static float att[8] __attribute__((aligned(16))), rel[8] __attribute__((aligned(16))), makeup[8] __attribute__((aligned(16)));
    static float mix[8] __attribute__((aligned(16)));
    static u32 on[8] __attribute__((aligned(16))), drv[8] __attribute__((aligned(16))), active[8] __attribute__((aligned(16)));
    static u32 pairm[8] __attribute__((aligned(16))), right[8] __attribute__((aligned(16))), keyed[8] __attribute__((aligned(16)));
    static u32 src[8], any_active, any_keyed;
    if (lanes_stale) {
        lanes_stale = 0;
        any_active = any_keyed = 0;
        for (u32 c = 0; c < MIX_CHANNELS; ++c) {
            u32 m = pair_right[c] ? c - 1u : c;
            const struct comp_coef *q = &coef[m];
            right[c] = pair_right[c] ? ~0u : 0u;
            pairm[c] = !pair_right[c] && c + 1 < MIX_CHANNELS && pair_right[c + 1] ? ~0u : 0u;
            on[c] = q->on && !pair_right[c] ? ~0u : 0u;
            drv[c] = q->drive ? ~0u : 0u;
            active[c] = q->on || q->drive ? ~0u : 0u;
            any_active |= active[c];
            /* k = 2^(5 Drive / 100) - 1, as drive_build. */
            k[c] = q->drive ? (float)(((u64)(u32)exp2_q24((s32)((s64)DRIVE_OCTAVES_Q24 * q->drive / MIX_PERCENT_MAX)) >> 8) - 65536u)
                                  / 65536.0f : 0.0f;
            thr[c] = (float)q->threshold / Q24_OCTAVE;
            slope[c] = (float)q->slope / 65536.0f;
            att[c] = (float)q->attack / 2147483648.0f;
            rel[c] = (float)q->release / 2147483648.0f;
            makeup[c] = (float)q->makeup / Q24_OCTAVE;
            mix[c] = (float)q->mix_q15 / Q15_ONE;
            src[c] = mix_now()->comp[m].key;
            keyed[c] = on[c] && src[c] && src[c] <= MIX_KEY_INPUT ? ~0u : 0u;
            any_keyed |= keyed[c];
            if (!on[c]) comp_gr[c] = 0.0f;
        }
    }
    if (!any_active) {
        for (u32 c = 0; c < MIX_CHANNELS; ++c) state[c].gr = 0;
        return;
    }
    /* Sidechain levels from other channels or the input, frame by frame. */
    static s32 side[AUDIO_BLOCK][MIX_CHANNELS] __attribute__((aligned(16)));
    if (any_keyed)
        for (u32 c = 0; c < MIX_CHANNELS; ++c)
            if (keyed[c])
                for (u32 f = 0; f < count; ++f) side[f][c] = magnitude(keys[f][src[c]]);

    /* In passes, so each holds few vectors: the driven channels; each
     * lane's detector (a pair's larger side, or its key), Q31 full scale;
     * the gain, four channels at a time; a right side takes its master's,
     * then the blend. */
    static float driven[AUDIO_BLOCK][MIX_CHANNELS] __attribute__((aligned(16)));
    static float level[AUDIO_BLOCK][MIX_CHANNELS] __attribute__((aligned(16)));
    const float32x4_t one = vdupq_n_f32(1.0f), zero = vdupq_n_f32(0.0f);
    for (u32 h = 0; h < MIX_CHANNELS; h += 4) {
        const float32x4_t kk = lanes_f(k + h);
        const uint32x4_t dd = lanes_u(drv + h);
        for (u32 f = 0; f < count; ++f) {
            float32x4_t d = vcvtq_f32_s32(vld1q_s32(x[f] + h));
            vst1q_f32(driven[f] + h, vbslq_f32(dd, drive_f(d, kk), d));
        }
    }
    {
        const uint32x4_t p0 = lanes_u(pairm), p1 = lanes_u(pairm + 4);
        for (u32 f = 0; f < count; ++f) {
            float32x4_t a0 = vabsq_f32(vld1q_f32(driven[f])), a1 = vabsq_f32(vld1q_f32(driven[f] + 4));
            float32x4_t n0 = vextq_f32(a0, a1, 1), n1 = vextq_f32(a1, zero, 1);
            float32x4_t e0 = vbslq_f32(p0, vmaxq_f32(a0, n0), a0), e1 = vbslq_f32(p1, vmaxq_f32(a1, n1), a1);
            vst1q_f32(level[f], vaddq_f32(e0, e0));
            vst1q_f32(level[f] + 4, vaddq_f32(e1, e1));
        }
    }
    for (u32 h = 0; h < MIX_CHANNELS; h += 4) {
        const uint32x4_t live = lanes_u(on + h), key = lanes_u(keyed + h);
        const float32x4_t th = vaddq_f32(lanes_f(thr + h), vdupq_n_f32(31.0f)), sl = lanes_f(slope + h);
        const float32x4_t at = lanes_f(att + h), re = lanes_f(rel + h), mk = lanes_f(makeup + h);
        float32x4_t gr = lanes_f(comp_gr + h);
        if (!(on[h] | on[h + 1] | on[h + 2] | on[h + 3])) {
            for (u32 f = 0; f < count; ++f) vst1q_f32(level[f] + h, one);
            continue;
        }
        for (u32 f = 0; f < count; ++f) {
            float32x4_t e = vld1q_f32(level[f] + h);
            if (any_keyed) e = vbslq_f32(key, vcvtq_f32_s32(vld1q_s32(side[f] + h)), e);
            /* Octaves over the threshold, the target reduction, its glide. */
            float32x4_t t = vmulq_f32(vmaxq_f32(vsubq_f32(log2_f(vmaxq_f32(e, one)), th), zero), sl);
            float32x4_t c = vbslq_f32(negative(vsubq_f32(t, gr)), re, at);
            gr = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vmlaq_f32(gr, vsubq_f32(t, gr), c)), live));
            vst1q_f32(level[f] + h, vbslq_f32(live, exp2_f(vsubq_f32(mk, gr)), one));
        }
        vst1q_f32(comp_gr + h, gr);
    }
    {
        const uint32x4_t r0 = lanes_u(right), r1 = lanes_u(right + 4), a0 = lanes_u(active), a1 = lanes_u(active + 4);
        const float32x4_t m0 = lanes_f(mix), m1 = lanes_f(mix + 4);
        const float32x4_t top = vdupq_n_f32(2147483648.0f), bottom = vdupq_n_f32(-2147483648.0f);
        for (u32 f = 0; f < count; ++f) {
            float32x4_t g0 = vld1q_f32(level[f]), g1 = vld1q_f32(level[f] + 4);
            g1 = vbslq_f32(r1, vextq_f32(g0, g1, 3), g1);
            g0 = vbslq_f32(r0, vextq_f32(one, g0, 3), g0);
            int32x4_t i0 = vld1q_s32(x[f]), i1 = vld1q_s32(x[f] + 4);
            float32x4_t d0 = vcvtq_f32_s32(i0), d1 = vcvtq_f32_s32(i1);
            float32x4_t w0 = vminq_f32(vmaxq_f32(vmulq_f32(vld1q_f32(driven[f]), g0), bottom), top);
            float32x4_t w1 = vminq_f32(vmaxq_f32(vmulq_f32(vld1q_f32(driven[f] + 4), g1), bottom), top);
            w0 = vmlaq_f32(d0, vsubq_f32(w0, d0), m0);
            w1 = vmlaq_f32(d1, vsubq_f32(w1, d1), m1);
            vst1q_s32(x[f], vbslq_s32(a0, vcvtq_s32_f32(w0), i0));
            vst1q_s32(x[f] + 4, vbslq_s32(a1, vcvtq_s32_f32(w1), i1));
        }
    }
    for (u32 c = 0; c < MIX_CHANNELS; ++c) state[c].gr = (s32)(comp_gr[c] * (float)Q24_OCTAVE);
}

/* The sends: each channel's source (Pre or Post, chosen by fx.c) at its
 * send levels. A pair's right side takes its master's sends. The CV holds
 * still through a block, so the routed levels are worked out once a block
 * (upstream: every frame, the same values). */
static s32 send_target[MIX_CHANNELS][MIX_SEND_ROUTES];
static void sends_targets(void)
{
    const struct mix_settings *m = mix_now();
    for (u32 c = 0; c < MIX_CHANNELS; ++c) {
        const struct mix_route *route = m->send_route[c];
        send_target[c][MIX_ROUTE_REVERB_SEND] = routed(m->reverb_send[c], &route[MIX_ROUTE_REVERB_SEND],
            send_input[c][MIX_ROUTE_REVERB_SEND], 0, MIX_PERCENT_MAX) * PERCENT_Q15;
        send_target[c][MIX_ROUTE_DELAY_SEND] = routed(m->delay_send[c], &route[MIX_ROUTE_DELAY_SEND],
            send_input[c][MIX_ROUTE_DELAY_SEND], 0, MIX_PERCENT_MAX) * PERCENT_Q15;
    }
}
/* The block's sends, Q31 (upstream: one frame at a time, each level
 * gliding every frame). Once every level has reached the end of its glide
 * the weights stay put through the block. */
static s32 sends[AUDIO_BLOCK][2];   /* reverb, delay */
static void sends_block(const s32 (*from)[MIX_CHANNELS], u32 count)
{
    s32 w[2][MIX_CHANNELS] __attribute__((aligned(16)));
    u32 moving = 0;
    for (u32 c = 0; c < MIX_CHANNELS; ++c) {
        if (pair_right[c]) continue;
        const s32 *now = send_now[c];
        moving |= (u32)(glide(now[MIX_ROUTE_REVERB_SEND], send_target[c][MIX_ROUTE_REVERB_SEND]) != now[MIX_ROUTE_REVERB_SEND]);
        moving |= (u32)(glide(now[MIX_ROUTE_DELAY_SEND], send_target[c][MIX_ROUTE_DELAY_SEND]) != now[MIX_ROUTE_DELAY_SEND]);
    }
    const int32x2_t floor = vdup_n_s32(-0x7fffffff);
    int32x4_t r0 = vdupq_n_s32(0), r1 = r0, d0 = r0, d1 = r0;
    for (u32 f = 0; f < count; ++f) {
        if (moving || !f) {
            for (u32 c = 0; c < MIX_CHANNELS; ++c) {
                s32 *now = send_now[c];
                if (pair_right[c]) {
                    /* A pair's right side at its master's levels. */
                    w[0][c] = w[0][c - 1];
                    w[1][c] = w[1][c - 1];
                    continue;
                }
                now[MIX_ROUTE_REVERB_SEND] = glide(now[MIX_ROUTE_REVERB_SEND], send_target[c][MIX_ROUTE_REVERB_SEND]);
                now[MIX_ROUTE_DELAY_SEND] = glide(now[MIX_ROUTE_DELAY_SEND], send_target[c][MIX_ROUTE_DELAY_SEND]);
                w[0][c] = now[MIX_ROUTE_REVERB_SEND];
                w[1][c] = now[MIX_ROUTE_DELAY_SEND];
            }
            r0 = vld1q_s32(w[0]); r1 = vld1q_s32(w[0] + 4);
            d0 = vld1q_s32(w[1]); d1 = vld1q_s32(w[1] + 4);
        }
        /* Exact 64-bit sums, then >> 15 held within +-(2^31 - 1). */
        int32x4_t a = vld1q_s32(from[f]), b = vld1q_s32(from[f] + 4);
        int64x2_t rev = vmull_s32(vget_low_s32(a), vget_low_s32(r0)), dly = vmull_s32(vget_low_s32(a), vget_low_s32(d0));
        rev = vmlal_s32(rev, vget_high_s32(a), vget_high_s32(r0));
        dly = vmlal_s32(dly, vget_high_s32(a), vget_high_s32(d0));
        rev = vmlal_s32(rev, vget_low_s32(b), vget_low_s32(r1));
        dly = vmlal_s32(dly, vget_low_s32(b), vget_low_s32(d1));
        rev = vmlal_s32(rev, vget_high_s32(b), vget_high_s32(r1));
        dly = vmlal_s32(dly, vget_high_s32(b), vget_high_s32(d1));
        int64x2_t both = vaddq_s64(vcombine_s64(vget_low_s64(rev), vget_low_s64(dly)),
                                   vcombine_s64(vget_high_s64(rev), vget_high_s64(dly)));
        vst1_s32(sends[f], vmax_s32(vqshrn_n_s64(both, 15), floor));
    }
}

/* The reverb for a block: the send down to 32 kHz, the ticks, and both
 * sides back up to 48 kHz into wet_block (Q30 sums, upstream's wet). Two
 * ticks every three frames, by phase. */
static void reverb_block(u32 count)
{
    for (u32 f = 0; f < count; ++f) down_line[DOWN_KEEP + f] = (s16)(sends[f][0] >> 16);
    u32 ph = phase, ticks = 0;
    const int16x8_t d0a = vld1q_s16(down_taps[0]), d0b = vld1q_s16(down_taps[0] + 8);
    const int16x8_t d1a = vld1q_s16(down_taps[1]), d1b = vld1q_s16(down_taps[1] + 8);
    for (u32 f = 0; f < count; ++f) {
        if (ph != 2) {
            int16x8_t a = vld1q_s16(down_line + f), b = vld1q_s16(down_line + f + 8);
            int16x8_t ta = ph ? d1a : d0a, tb = ph ? d1b : d0b;
            int32x4_t acc = vmull_s16(vget_low_s16(a), vget_low_s16(ta));
            acc = vmlal_s16(acc, vget_high_s16(a), vget_high_s16(ta));
            acc = vmlal_s16(acc, vget_low_s16(b), vget_low_s16(tb));
            acc = vmlal_s16(acc, vget_high_s16(b), vget_high_s16(tb));
            int32x2_t sum = vpadd_s32(vget_low_s32(acc), vget_high_s32(acc));
            sum = vpadd_s32(sum, sum);
            vst1_lane_s16(&tick_in[ticks++], vqmovn_s32(vcombine_s32(vshr_n_s32(sum, 15), sum)), 0);
        }
        ph = ph == 2 ? 0 : ph + 1;
    }
    memmove(down_line, down_line + count, DOWN_KEEP * sizeof(down_line[0]));
    for (u32 t = 0; t < ticks; ++t) {
        s32 l, r;
        reverb_tick(tick_in[t], &l, &r);
        up_line[0][MIX_UP_TAPS + t] = (s16)l;
        up_line[1][MIX_UP_TAPS + t] = (s16)r;
    }
    ph = phase;
    for (u32 f = 0, t = 0; f < count; ++f) {
        if (ph != 2) ++t;
        /* The twelve up to the newest tick. */
        const s16 *l = &up_line[0][t], *r = &up_line[1][t];
        int16x8_t k8 = vld1q_s16(up_taps[ph]);
        int16x4_t k4 = vld1_s16(up_taps[ph] + 8);
        int16x8_t l8 = vld1q_s16(l), r8 = vld1q_s16(r);
        int32x4_t al = vmull_s16(vget_low_s16(l8), vget_low_s16(k8));
        int32x4_t ar = vmull_s16(vget_low_s16(r8), vget_low_s16(k8));
        al = vmlal_s16(al, vget_high_s16(l8), vget_high_s16(k8));
        ar = vmlal_s16(ar, vget_high_s16(r8), vget_high_s16(k8));
        al = vmlal_s16(al, vld1_s16(l + 8), k4);
        ar = vmlal_s16(ar, vld1_s16(r + 8), k4);
        int32x2_t sl = vpadd_s32(vget_low_s32(al), vget_high_s32(al));
        int32x2_t sr = vpadd_s32(vget_low_s32(ar), vget_high_s32(ar));
        vst1_s32(wet_block[f], vpadd_s32(sl, sr));
        ph = ph == 2 ? 0 : ph + 1;
    }
    phase = ph;
    for (u32 s = 0; s < 2; ++s) memmove(up_line[s], up_line[s] + ticks, MIX_UP_TAPS * sizeof(up_line[0][0]));
}

static u32 delay_frame(s32 *mix, u32 f)
{
    u32 peak = 0;
    reverb_level_q15 = glide(reverb_level_q15, reverb_level_target);
    delay_level_q15 = glide(delay_level_q15, delay_level_target);
    feedback_q15 = glide(feedback_q15, feedback_target);
    const s32 *wet = wet_block[f];

    /* Ping-pong delay: the send enters the left line, each line feeds the other. */
    u32 target = delay_target_q8;
    for (u32 s = 0; s < 2; ++s) {
        /* The last step's fraction: upstream's shift stops up to 8 frames short of a longer time. */
        s32 d = (s32)(target - delay_q8[s]), step = d >> DELAY_GLIDE_SHIFT;
        delay_q8[s] += (u32)(step ? step : (d > 0) - (d < 0));
    }
    /* Each repeat, the first too, passes the low and high cut. */
    s32 echo[2];
    for (u32 s = 0; s < 2; ++s) {
        s32 v = delay_read(s);
        high_state[s] += ((v - high_state[s]) * high_coef) >> 15;
        v = high_state[s];
        low_state[s] += ((v - low_state[s]) * low_coef) >> 15;
        echo[s] = v - low_state[s];
    }
    s32 into_left = (sends[f][1] >> 17) + ((echo[1] * feedback_q15) >> 15);
    s32 into_right = (echo[0] * feedback_q15) >> 15;
    delay_ram[0][delay_write & DELAY_MASK] = (s16)soft_clip(into_left);
    delay_ram[1][delay_write & DELAY_MASK] = (s16)soft_clip(into_right);
    delay_write = (delay_write + 1u) & DELAY_MASK;

    for (u32 s = 0; s < 2; ++s) {
        /* Q15 returns times Q15 levels, to Q31; the delay's send has 6 dB of headroom to restore. */
        s64 add = (s64)(wet[s] >> 15) * reverb_level_q15 * 2 + (s64)echo[s] * delay_level_q15 * 4;
        mix[s] = sat32((s64)mix[s] + add);
        u32 w = (u32)magnitude(wet[s] >> 15), e = (u32)magnitude(echo[s]);
        if (w > peak) peak = w;
        if (e > peak) peak = e;
    }
    return peak;
}

/* The mix compressor over the block, specialised as the channels' are. */
static inline __attribute__((always_inline)) void master_block(s32 (*mix)[2], u32 count, const u16 *curve, u32 on,
    const s32 *key)
{
    const struct comp_coef *k = &coef[MIX_CHANNELS];
    const u32 slope = k->slope, attack = k->attack, release = k->release;
    const s32 threshold = k->threshold, makeup = k->makeup, mix_q15 = k->mix_q15;
    s32 gr = state[MIX_CHANNELS].gr;
    for (u32 f = 0; f < count; ++f) {
        s32 d0 = mix[f][0], d1 = mix[f][1];
        s32 x0 = curve ? drive(curve, d0, 0) : d0, x1 = curve ? drive(curve, d1, 0) : d1;
        s32 g = GAIN_ONE_Q24;
        if (on) {
            /* Both sides share one detector. */
            s32 a = magnitude(x0), b = magnitude(x1);
            s32 detect = key ? key[f * (MIX_KEY_INPUT + 1)] : a > b ? a : b;
            u32 m = detect < 0 ? (u32)-(detect + 1) : (u32)detect;
            s32 over = (m > 0 ? log2_q24(m) - (31 << 24) : LEVEL_FLOOR) - threshold;
            s32 target = over > 0 ? (s32)(((s64)over * slope) >> 16) : 0;
            gr = smooth(gr, target, target > gr ? attack : release);
            g = exp2_q24(makeup - gr);
        }
        mix[f][0] = mix_q15 == Q15_ONE ? apply(x0, g) : blend(d0, apply(x0, g), mix_q15);
        mix[f][1] = mix_q15 == Q15_ONE ? apply(x1, g) : blend(d1, apply(x1, g), mix_q15);
    }
    state[MIX_CHANNELS].gr = on ? gr : 0;
}

u32 ta_mix_bus(s32 (*mix)[2], const s32 (*send)[MIX_CHANNELS], u32 count, u32 effects)
{
    u32 peak = 0;
    if (effects) {
        sends_targets();
        sends_block(send, count);
        ta_mix_monitor(sends, count);
        prof_to(PS_RVB);
        if (ta_reverb80((const s32 (*)[2])sends, wet_block, count)) algo_now = MIX_ALGOS;
        else reverb_block(count);
        prof_to(PS_DLY);
        for (u32 f = 0; f < count; ++f) {
            u32 p = delay_frame(mix[f], f);
            if (p > peak) peak = p;
        }
    }
    prof_to(PS_MCP);
    const struct comp_coef *k = &coef[MIX_CHANNELS];
    if (k->on || k->drive) {
        const u16 *curve = k->drive && drive_built[MIX_CHANNELS] == k->drive + 1u ? drive_curve[MIX_CHANNELS] : 0;
        u32 source = mix_now()->master.key;
        const s32 *key = source && source <= MIX_KEY_INPUT ? &keys[0][source] : 0;
        if (!k->on) master_block(mix, count, curve, 0, 0);
        else if (curve) master_block(mix, count, curve, 1, key);
        else master_block(mix, count, 0, 1, key);
    }
    prof_to(PS_MIX);
    return peak;
}

/* Once a block, before the stages: an edit's routes and coefficients (all
 * pending compressors: upstream sets one a frame), the routed settings every
 * 32 frames, the delay's tempo every 256. */
void ta_mix_block(u32 count)
{
    u32 before = frames;
    frames += count;
    if (dirty) { dirty = 0; settings_changed(); control(); }
    else if ((before ^ frames) & ~CONTROL_MASK) control();
    while (comps_pending) {
        u32 c = (u32)__builtin_ctz(comps_pending);
        comps_pending &= comps_pending - 1u;
        comp_coef_set(&coef[c], c < MIX_CHANNELS ? &mix_now()->comp[c] : &mix_now()->master);
        lanes_stale = 1;
    }
    if ((before ^ frames) & ~DELAY_RETARGET_MASK) delay_target();
}

void mix_fx_flush(void)
{
    memset(reverb_ram, 0, sizeof(reverb_ram));
    memset(delay_ram, 0, sizeof(delay_ram));
    memset(chorus_ram, 0, sizeof(chorus_ram));
    memset(down_line, 0, sizeof(down_line));
    memset(up_line, 0, sizeof(up_line));
    memset(wet_block, 0, sizeof(wet_block));
    memset(sends, 0, sizeof(sends));
    memset(comb_lp, 0, sizeof(comb_lp));
    memset(state, 0, sizeof(state));
    memset(comp_gr, 0, sizeof(comp_gr));
    gated_lp = 0;
    low_state[0] = low_state[1] = high_state[0] = high_state[1] = 0;
    clearing = 0;
}

void mix_fx_init(void)
{
    static const struct mix_comp_set comp_default = {0, 20, 4, 3, 0, 0, MIX_PERCENT_MAX, MIX_KEY_SELF};
    mix_defaults.format = MIX_FORMAT;
    for (u32 c = 0; c < MIX_CHANNELS; ++c) mix_defaults.comp[c] = comp_default;
    mix_defaults.master = comp_default;
    mix_defaults.reverb_decay = 25; mix_defaults.reverb_damp = 20; mix_defaults.reverb_level = 80;
    mix_defaults.delay_division = 5; mix_defaults.delay_feedback = 45; mix_defaults.delay_low = 3;
    mix_defaults.delay_high = 2; mix_defaults.delay_level = 80; mix_defaults.delay_tempo = MIX_TEMPO_MIDI;
    mix_defaults.reverb_algo = 1; mix_defaults.chorus_rate = 40; mix_defaults.chorus_depth = 50;
    for (u32 n = 0; n < MIX_BUS_ROUTES; ++n) mix_defaults.bus_route[n].source = EQ_CV_SOURCE_OFF;
    for (u32 c = 0; c < MIX_CHANNELS; ++c)
        for (u32 n = 0; n < MIX_SEND_ROUTES; ++n) mix_defaults.send_route[c][n].source = EQ_CV_SOURCE_OFF;
    reverb_layout();
    reverb_constants();
    for (u32 p = 0; p < 2; ++p)
        for (u32 i = 0; i < MIX_DOWN_TAPS; ++i) down_taps[p][i] = mix_down_fir[p][MIX_DOWN_TAPS - 1u - i];
    for (u32 p = 0; p < 3; ++p)
        for (u32 i = 0; i < MIX_UP_TAPS; ++i) up_taps[p][i] = mix_up_fir[p][MIX_UP_TAPS - 1u - i];
    settings_changed();
    while (comps_pending) {
        u32 c = (u32)__builtin_ctz(comps_pending);
        comps_pending &= comps_pending - 1u;
        comp_coef_set(&coef[c], c < MIX_CHANNELS ? &mix_now()->comp[c] : &mix_now()->master);
        lanes_stale = 1;
    }
    division_now = MIX_DIVISIONS; decay_now = 0;
    control();
    reverb_level_q15 = reverb_level_target; delay_level_q15 = delay_level_target; feedback_q15 = feedback_target;
    delay_q8[0] = delay_q8[1] = delay_target_q8;
}
