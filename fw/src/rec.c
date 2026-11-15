#include "rec.h"
#include "mix_fx.h"
#include "cv.h"
#include "cal.h"
#include "mmu.h"
#include "preset.h"
#include "samples.h"
#include "storage.h"
#include "systime.h"

#include <arm_neon.h>
#include <string.h>

struct rec_setup rec_setup = {
    .dest = RDEST_NONE, .mode = RMODE_ONCE, .rate = RRATE_48, .naming = RNAME_NEW, .side = RSIDE_STEREO,
    .thresh = -40,
};

volatile float rec_peak[2];
volatile uint32_t rec_us_max, rec_us_last;

/* ---- Interrupt side ---- */

/* Half-band decimator, 79 taps (Kaiser, beta 10): flat to 0.21 fs, -99 dB
 * from 0.29 fs, so 192 -> 96 kHz keeps 0-40 kHz and 96 -> 48 kHz 0-20 kHz
 * free of aliases. Odd taps either side of the centre. */
#define HB_N    79
#define HB_HIST (HB_N - 1)
#define HB_MID  (HB_HIST / 2)
#define IN_MAX  256
static const float hb_c[22] __attribute__((aligned(16))) = {
    3.173185028e-01f, -1.031627827e-01f, 5.886952185e-02f, -3.898339160e-02f, 2.738391272e-02f,
    -1.969650089e-02f, 1.424583258e-02f, -1.024652369e-02f, 7.271950965e-03f, -5.060132551e-03f,
    3.432375758e-03f, -2.256187514e-03f, 1.427563869e-03f, -8.623460063e-04f, 4.919071844e-04f,
    -2.608317911e-04f, 1.253927504e-04f, -5.223469843e-05f, 1.700865785e-05f, -2.898653697e-06f,
    0.49999972f, 0.0f      /* the centre */
};
#define HB_CENTRE hb_c[20]

/* Planar: b[channel][HB_HIST history + new frames], padded so a group of
 * eight outputs may read past the last frame. */
#define HB_PAD  16
struct hb {
    float b[2][HB_HIST + IN_MAX + HB_PAD] __attribute__((aligned(16)));
    unsigned phase;
};
static struct hb hb1, hb2;

/* One tap of two groups of four outputs: a0/b0 walk out from the first
 * group's centre, a1/b1 from the second's (vld2: every second frame). */
#define HB_TAP(d, lane)                                 \
    "vld2.32 {d20-d23}, [%[a0]], %[dn]\n\t"             \
    "vld2.32 {d24-d27}, [%[b0]], %[up]\n\t"             \
    "vld2.32 {d28-d31}, [%[a1]], %[dn]\n\t"             \
    "vld2.32 {d12-d15}, [%[b1]], %[up]\n\t"             \
    "vadd.f32 q10, q10, q12\n\t"                       \
    "vadd.f32 q14, q14, q6\n\t"                        \
    "vmla.f32 q8, q10, " d "[" lane "]\n\t"            \
    "vmla.f32 q9, q14, " d "[" lane "]\n\t"

/* Eight outputs from centres q and q + 8 into o and o + 4, summed as the
 * four-output loop in hb_run does (independent chains, same order). */
static void hb_eight(const float *q, float *o)
{
    const float *a0 = q - 1, *b0 = q + 1, *a1 = q + 7, *b1 = q + 9, *k = hb_c;
    const int dn = -8, up = 8;

    __asm__ volatile(
        "vld1.32 {d0-d3}, [%[k]:128]!\n\t"
        "vld1.32 {d4-d7}, [%[k]:128]!\n\t"
        "vld1.32 {d8-d10}, [%[k]:64]\n\t"
        "vld2.32 {d20-d23}, [%[q]]\n\t"
        "vld2.32 {d24-d27}, [%[q8]]\n\t"
        "vmul.f32 q8, q10, d10[0]\n\t"
        "vmul.f32 q9, q12, d10[0]\n\t"
        HB_TAP("d0", "0") HB_TAP("d0", "1") HB_TAP("d1", "0") HB_TAP("d1", "1")
        HB_TAP("d2", "0") HB_TAP("d2", "1") HB_TAP("d3", "0") HB_TAP("d3", "1")
        HB_TAP("d4", "0") HB_TAP("d4", "1") HB_TAP("d5", "0") HB_TAP("d5", "1")
        HB_TAP("d6", "0") HB_TAP("d6", "1") HB_TAP("d7", "0") HB_TAP("d7", "1")
        HB_TAP("d8", "0") HB_TAP("d8", "1") HB_TAP("d9", "0") HB_TAP("d9", "1")
        "vst1.32 {d16-d17}, [%[o]]!\n\t"
        "vst1.32 {d18-d19}, [%[o]]\n\t"
        : [a0] "+r"(a0), [b0] "+r"(b0), [a1] "+r"(a1), [b1] "+r"(b1), [k] "+r"(k), [o] "+r"(o)
        : [q] "r"(q), [q8] "r"(q + 8), [dn] "r"(dn), [up] "r"(up)
        : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d12", "d13", "d14", "d15",
          "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29",
          "d30", "d31", "memory");
}

/* Filters the n frames placed at b[c][HB_HIST..] into every second output,
 * eight (hb_eight) or four at a time. */
static unsigned hb_run(struct hb *s, unsigned n, float *out[2])
{
    unsigned total = HB_HIST + n, p = s->phase, m = 0;

    if (total >= HB_N + p) {
        m = (total - HB_N - p) / 2u + 1u;
    }
    for (int c = 0; c < 2; c++) {
        unsigned i = 0;
        for (; i + 4u < m; i += 8) {
            hb_eight(s->b[c] + p + 2u * i + HB_MID, out[c] + i);
        }
        for (; i < m; i += 4) {
            const float *q = s->b[c] + p + 2u * i + HB_MID;
            float32x4_t acc = vmulq_n_f32(vld2q_f32(q).val[0], HB_CENTRE);
            for (int k = 0; k < 20; k++) {
                float32x4_t a = vld2q_f32(q - (2 * k + 1)).val[0], b = vld2q_f32(q + (2 * k + 1)).val[0];
                acc = vmlaq_n_f32(acc, vaddq_f32(a, b), hb_c[k]);
            }
            vst1q_f32(out[c] + i, acc);
        }
        for (i = 0; i < HB_HIST + 2u; i += 4) {
            vst1q_f32(s->b[c] + i, vld1q_f32(s->b[c] + i + n));
        }
    }
    s->phase = p + 2u * m - n;
    return m;
}

/* The capture ring: processed frames at the record rate, 24-bit values
 * left-justified; 2.7 s at 192 kHz for the main loop to move them out. */
#define F_BITS  19
#define F_SIZE  (1u << F_BITS)
#define F_MASK  (F_SIZE - 1u)
#define F_SLACK 16384u
static int32_t fring[F_SIZE][2];
static volatile uint32_t fw, cap_start;
static volatile uint8_t cap_want, cap_reset;
static uint8_t cap_on, cap_rate;
static float hp_x[2], hp_y[2];

/* Arming: 1 = watching for a start, 2 = recording (watching for stops). */
static volatile uint8_t watch;
static volatile uint8_t req_start, req_stop;
static volatile uint32_t trig_at;
static volatile uint8_t trig_new;
#define EV_MAX 4
static volatile uint32_t ev_at[EV_MAX], ev_n;
static uint8_t cv_high;

static int32_t r48[64][2];
static unsigned w48;
/* From the main loop: level and threshold as gains, whether a threshold may
 * start a take (only after arming, even when *HOT*). */
static volatile float in_gain = 1.0f, thr_gain = 2.0f;
static volatile uint8_t thr_ok;
/* A channel phase-modulated by an input: the 48 kHz stream is needed. */
static volatile uint8_t in_need;
volatile int rec_level_on;
volatile int32_t rec_level[2];
volatile uint32_t rec_level_epoch;

static float db_lin(int db)
{
    float g = 1.0f;
    float step = db < 0 ? 0.891250938f : 1.122018454f;

    for (int i = db < 0 ? -db : db; i > 0; i--) {
        g *= step;
    }
    return g;
}

static void stop_event(uint32_t at)
{
    ev_at[ev_n % EV_MAX] = at;
    ev_n = ev_n + 1;
}

static void store(uint32_t w, float32x4_t l, float32x4_t r)
{
    const float32x4_t full = vdupq_n_f32(2147483648.0f);
    const int32x4_t half = vdupq_n_s32(0x80), mask = vdupq_n_s32(~0xff);
    int32x4x2_t q = { { vandq_s32(vqaddq_s32(vcvtq_s32_f32(vmulq_f32(l, full)), half), mask),
                        vandq_s32(vqaddq_s32(vcvtq_s32_f32(vmulq_f32(r, full)), half), mask) } };

    if ((w & F_MASK) <= F_SIZE - 4u) {
        vst2q_s32(fring[w & F_MASK], q);
        return;
    }
    int32_t t[2][4];
    vst1q_s32(t[0], q.val[0]);
    vst1q_s32(t[1], q.val[1]);
    for (int k = 0; k < 4; k++) {
        fring[(w + (uint32_t)k) & F_MASK][0] = t[0][k];
        fring[(w + (uint32_t)k) & F_MASK][1] = t[1][k];
    }
}

/* n frames (planar) at `rate` through high-pass and level into the ring,
 * four at a time (a last partial group is written but not counted). */
static void capture(float *l, float *r, unsigned n, uint32_t rate)
{
    const struct rec_setup *rs = &rec_setup;
    float g = in_gain;
    float a = 1.0f - 6.2831853f * 4.0f / (float)rate;       /* 4 Hz high-pass */
    float32x4_t pl = vdupq_n_f32(0.0f), pr = pl;
    uint32_t w = fw;
    int look = watch == 1 && thr_ok && rs->thresh < 0, side = rs->side;

    if (rs->hpf) {
        /* One pole each side, L and R as a pair, four frames per load. */
        float32x2_t y = vld1_f32(hp_y), x1 = vld1_f32(hp_x);
        for (unsigned i = 0; i < n; i += 4) {
            float32x4x2_t z = vzipq_f32(vld1q_f32(l + i), vld1q_f32(r + i));
            float32x2_t f[4] = { vget_low_f32(z.val[0]), vget_high_f32(z.val[0]), vget_low_f32(z.val[1]),
                                 vget_high_f32(z.val[1]) };
            for (int k = 0; k < 4; k++) {
                y = vmul_n_f32(vadd_f32(y, vsub_f32(f[k], x1)), a);
                x1 = f[k];
                f[k] = y;
                if (i + (unsigned)k + 1 == n) {
                    vst1_f32(hp_y, y);
                    vst1_f32(hp_x, x1);
                }
            }
            float32x4x2_t u = vuzpq_f32(vcombine_f32(f[0], f[1]), vcombine_f32(f[2], f[3]));
            vst1q_f32(l + i, u.val[0]);
            vst1q_f32(r + i, u.val[1]);
        }
    }
    for (unsigned i = 0; i < n; i += 4) {
        float32x4_t vl = vmulq_n_f32(vld1q_f32(l + i), g), vr = vmulq_n_f32(vld1q_f32(r + i), g);
        float32x4_t al = vabsq_f32(vl), ar = vabsq_f32(vr);
        if (n - i < 4) {
            static const uint32_t keep[4][4] = { { 0 }, { ~0u }, { ~0u, ~0u }, { ~0u, ~0u, ~0u } };
            uint32x4_t k = vld1q_u32(keep[n - i]);
            al = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(al), k));
            ar = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(ar), k));
        }
        pl = vmaxq_f32(pl, al);
        pr = vmaxq_f32(pr, ar);
        if (look) {
            float32x4_t v = side == RSIDE_LEFT ? al : side == RSIDE_RIGHT ? ar : vmaxq_f32(al, ar);
            uint32x4_t hit = vcgtq_f32(v, vdupq_n_f32(thr_gain));
            uint32x2_t h2 = vorr_u32(vget_low_u32(hit), vget_high_u32(hit));
            if (vget_lane_u32(vpmax_u32(h2, h2), 0)) {
                uint32_t b[4];
                vst1q_u32(b, hit);
                for (int k = 0; k < 4; k++) {
                    if (b[k]) {
                        trig_at = w + (uint32_t)k;
                        break;
                    }
                }
                trig_new = 1;
                watch = 2;
                look = 0;
            }
        }
        store(w, vl, vr);
        w += n - i < 4 ? n - i : 4u;
    }
    fw = w;
    float32x2_t m = vpmax_f32(vget_low_f32(pl), vget_high_f32(pl));
    float lp = vget_lane_f32(vpmax_f32(m, m), 0);
    m = vpmax_f32(vget_low_f32(pr), vget_high_f32(pr));
    float rp = vget_lane_f32(vpmax_f32(m, m), 0);
    rec_peak[0] = lp > rec_peak[0] * BLOCK_DECAY(0.97f) ? lp : rec_peak[0] * BLOCK_DECAY(0.97f);
    rec_peak[1] = rp > rec_peak[1] * BLOCK_DECAY(0.97f) ? rp : rec_peak[1] * BLOCK_DECAY(0.97f);
}

static void copy_f(float *d, const float *s, unsigned n)
{
    for (unsigned i = 0; i < n; i += 4) {
        vst1q_f32(d + i, vld1q_f32(s + i));
    }
}

static void cv_check(void)
{
    int in = rec_setup.thresh;
    float v;

    if (in <= 0 || in > CV_INPUTS) {
        cv_high = 1;
        return;
    }
    v = cv_volts[in - 1];
    if (!cv_high && v > 1.6f) {
        cv_high = 1;
        if (watch == 1) {
            trig_at = fw;
            trig_new = 1;
            watch = 2;
        } else if (watch == 2) {
            stop_event(fw);
        }
    } else if (cv_high && v < 1.4f) {
        cv_high = 0;
    }
}

void rec_input(const volatile int32_t (*x)[2], unsigned n, int32_t in48[][2])
{
    static float f48[2][IN_MAX / 4 + 8] __attribute__((aligned(16)));
    float *f192[2] = { hb1.b[0] + HB_HIST, hb1.b[1] + HB_HIST }, *f96[2] = { hb2.b[0] + HB_HIST, hb2.b[1] + HB_HIST };
    float *o48[2] = { f48[0], f48[1] };
    uint32_t t0 = time_ticks();
    unsigned n96, n48;
    const float32x4_t sc = vdupq_n_f32(1.0f / 2147483648.0f);

    if (n > IN_MAX) {
        x += n - IN_MAX;
        n = IN_MAX;
    }
    if (!cap_want && !in_need && !rec_level_on) {
        cap_on = 0;
        req_start = req_stop = 0;
        rec_peak[0] *= BLOCK_DECAY(0.9f);
        rec_peak[1] *= BLOCK_DECAY(0.9f);
        memset(in48, 0, AUDIO_BLOCK * sizeof(in48[0]));
        return;
    }
    /* x is the uncached DMA ring: copied out with LDM only, never NEON or
     * VFP loads (they stall the Cortex-A8 for good; see cv.c row_copy). A
     * plain (float)x[i][0] compiles to VLDR, and n is any count on hardware
     * (McASP1 runs from its own clock), so the odd frame goes by LDM too. */
    static int32_t raw[IN_MAX + 4][2] __attribute__((aligned(16)));
    unsigned i = 0;
    for (; i + 2 <= n; i += 2) {
        __asm__ volatile("ldm %1, {r4-r7}\n\tstm %0, {r4-r7}" : : "r"(raw[i]), "r"(x[i]) : "r4", "r5", "r6", "r7",
                         "memory");
    }
    if (i < n) {
        __asm__ volatile("ldm %1, {r4-r5}\n\tstm %0, {r4-r5}" : : "r"(raw[i]), "r"(x[i]) : "r4", "r5", "memory");
    }
    /* Stock's input DC calibration, first thing at the raw rate: offset -
     * raw, saturating. The analog input stage inverts; this puts it back. */
    {
        const int32x4_t o = { cal_live.in_off[0], cal_live.in_off[1], cal_live.in_off[0], cal_live.in_off[1] };
        for (i = 0; i < n; i += 2) {
            vst1q_s32(raw[i], cal_in_words(o, vld1q_s32(raw[i])));
        }
    }
    if (rec_level_on) {
        int64_t t[2] = { 0, 0 };
        for (i = 0; i < n; i++) {
            t[0] += raw[i][0];
            t[1] += raw[i][1];
        }
        for (int c = 0; c < 2 && n; c++) {
            int64_t q = t[c] / (int64_t)n;
            rec_level[c] = (int32_t)(q * (int64_t)n > t[c] ? q - 1 : q);
        }
        rec_level_epoch++;
        if (!cap_want && !in_need) {
            cap_on = 0;
            rec_peak[0] *= BLOCK_DECAY(0.9f);
            rec_peak[1] *= BLOCK_DECAY(0.9f);
            memset(in48, 0, AUDIO_BLOCK * sizeof(in48[0]));
            return;
        }
    }
    for (i = 0; i < n; i += 4) {
        int32x4x2_t v = vld2q_s32(raw[i]);
        vst1q_f32(f192[0] + i, vmulq_f32(vcvtq_f32_s32(v.val[0]), sc));
        vst1q_f32(f192[1] + i, vmulq_f32(vcvtq_f32_s32(v.val[1]), sc));
    }
    if (cap_want && (!cap_on || cap_reset)) {
        cap_on = 1;
        cap_reset = 0;
        cap_rate = rec_setup.rate;
        cap_start = fw;
        hp_x[0] = hp_x[1] = hp_y[0] = hp_y[1] = 0.0f;
    } else if (!cap_want) {
        cap_on = 0;
        rec_peak[0] *= BLOCK_DECAY(0.9f);
        rec_peak[1] *= BLOCK_DECAY(0.9f);
    }
    if (cap_on) {
        if (req_start && watch == 1) {
            trig_at = fw;
            trig_new = 1;
            watch = 2;
        }
        if (req_stop && watch == 2) {
            stop_event(fw);
        }
        cv_check();
    }
    req_start = req_stop = 0;
    /* The 192 kHz frames are captured from a copy: the filter reads them. */
    if (cap_on && cap_rate == RRATE_192) {
        static float c192[2][IN_MAX + 4] __attribute__((aligned(16)));
        copy_f(c192[0], f192[0], n);
        copy_f(c192[1], f192[1], n);
        capture(c192[0], c192[1], n, 192000);
    }
    n96 = hb_run(&hb1, n, f96);
    if (cap_on && cap_rate == RRATE_96) {
        static float c96[2][IN_MAX / 2 + 8] __attribute__((aligned(16)));
        copy_f(c96[0], f96[0], n96);
        copy_f(c96[1], f96[1], n96);
        capture(c96[0], c96[1], n96, 96000);
    }
    n48 = hb_run(&hb2, n96, o48);
    for (unsigned k = 0; k < n48; k += 4) {
        int32_t t[2][4];
        vst1q_s32(t[0], vcvtq_s32_f32(vmulq_n_f32(vld1q_f32(f48[0] + k), 2147483648.0f)));
        vst1q_s32(t[1], vcvtq_s32_f32(vmulq_n_f32(vld1q_f32(f48[1] + k), 2147483648.0f)));
        for (unsigned j = 0; j < 4 && k + j < n48; j++) {
            r48[w48 & 63u][0] = t[0][j];
            r48[w48++ & 63u][1] = t[1][j];
        }
    }
    if (cap_on && cap_rate == RRATE_48) {
        capture(f48[0], f48[1], n48, 48000);
    }
    for (unsigned f = 0; f < AUDIO_BLOCK; f++) {
        in48[f][0] = r48[(w48 - AUDIO_BLOCK + f) & 63u][0];
        in48[f][1] = r48[(w48 - AUDIO_BLOCK + f) & 63u][1];
    }
    rec_us_last = (time_ticks() - t0) / 24u;
    if (rec_us_last > rec_us_max) {
        rec_us_max = rec_us_last;
    }
}

/* The monitor from the main loop: each input's gain to the mix's left and
 * right (mon[input][side]) and its sends (reverb, delay); zero when off. */
static volatile float mon[2][2], mon_send[2][2];

void rec_monitor(int32_t mix[][2], unsigned frames)
{
    float ll = mon[0][0], lr = mon[0][1], rl = mon[1][0], rr = mon[1][1];

    if (ll == 0.0f && lr == 0.0f && rl == 0.0f && rr == 0.0f) {
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        int32_t (*m)[2] = &mix[f];
        const int32_t *in = r48[(w48 - frames + f) & 63u];
        float32x2_t v = vcvt_f32_s32(vld1_s32(*m));
        float32x2_t x = vcvt_f32_s32(vld1_s32(in));
        float32x2_t a = { ll, lr }, b = { rl, rr };
        a = vmla_lane_f32(vmul_lane_f32(a, x, 0), b, x, 1);
        vst1_s32(*m, vcvt_s32_f32(vadd_f32(v, a)));
    }
}

void rec_monitor_send(int32_t send[][2], unsigned frames)
{
    static float prev[2][2];
    float g[2][2] = { { mon_send[0][0], mon_send[0][1] }, { mon_send[1][0], mon_send[1][1] } };
    const int32x2_t floor = vdup_n_s32(-0x7fffffff);

    if (g[0][0] == 0.0f && g[0][1] == 0.0f && g[1][0] == 0.0f && g[1][1] == 0.0f &&
        prev[0][0] == 0.0f && prev[0][1] == 0.0f && prev[1][0] == 0.0f && prev[1][1] == 0.0f) {
        return;
    }
    /* Gains ramp over the block, as the channel sends glide. */
    float32x2_t a = { prev[0][0], prev[0][1] }, b = { prev[1][0], prev[1][1] };
    float32x2_t da = vmul_n_f32(vsub_f32((float32x2_t){ g[0][0], g[0][1] }, a), 1.0f / (float)frames);
    float32x2_t db = vmul_n_f32(vsub_f32((float32x2_t){ g[1][0], g[1][1] }, b), 1.0f / (float)frames);
    for (unsigned f = 0; f < frames; f++) {
        const int32_t *in = r48[(w48 - frames + f) & 63u];
        float32x2_t x = vcvt_f32_s32(vld1_s32(in));
        a = vadd_f32(a, da);
        b = vadd_f32(b, db);
        float32x2_t s = vmla_lane_f32(vmul_lane_f32(a, x, 0), b, x, 1);
        vst1_s32(send[f], vmax_s32(vqadd_s32(vld1_s32(send[f]), vcvt_s32_f32(s)), floor));
    }
    memcpy(prev, g, sizeof(prev));
}

/* ---- Main loop side ---- */

static int state = REC_IDLE, metering, explicit_arm;
static const char *message;

/* The take: planes in sample memory from `base`; s = first ring frame. */
static struct {
    uint8_t *base;
    uint32_t bytes, cap, rate, pre, s, cp, wi, total, len, n, rot;
    int32_t *p[2];
    uint8_t planes, mode, side, pass, loop_set, short_mem;
    int left, right, ready;
    uint32_t ev_rd;
} tk;

static uint32_t plane_bytes(uint32_t frames)
{
    return ((frames + 2u * SAMPLE_GUARD) * 4u + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
}

int rec_state(void)
{
    return state;
}

int rec_armed(void)
{
    return state == REC_ARMED && explicit_arm;
}

static void arm(int on, int by_hand)
{
    if (on && state == REC_IDLE) {
        trig_new = 0;
        state = REC_ARMED;
        cap_want = 1;
        watch = 1;
    } else if (!on && state == REC_ARMED && !rec_setup.hot) {
        watch = 0;
        state = REC_IDLE;
    }
    if (by_hand) {
        explicit_arm = on && state == REC_ARMED;
    }
    thr_ok = (uint8_t)explicit_arm;
}

void rec_arm(int on)
{
    arm(on, 1);
}

int rec_button(void)
{
    if (state == REC_FINISHING || state == REC_SAVING) {
        return 1;
    }
    if (state == REC_ARMED) {
        req_start = 1;
        return 1;
    }
    if (state == REC_RECORDING) {
        req_stop = 1;
        return 1;
    }
    return 0;
}

void rec_metering(int on)
{
    metering = on;
}

float rec_thresh_lin(void)
{
    return rec_setup.thresh < 0 ? db_lin(rec_setup.thresh) : 0.0f;
}

uint32_t rec_frames(void)
{
    if (state != REC_RECORDING) {
        return tk.n;
    }
    if (tk.mode == RMODE_ONCE) {
        return tk.wi;
    }
    if (tk.mode == RMODE_CIRCULAR) {
        return tk.total < tk.cap ? tk.total : tk.cap;
    }
    return tk.pass ? tk.len : tk.wi;
}

uint32_t rec_capacity(void)
{
    return tk.mode >= RMODE_SOS && tk.pass ? tk.len : tk.cap;
}

uint32_t rec_rate_hz(void)
{
    return tk.rate ? tk.rate : 48000u << rec_setup.rate;
}

const char *rec_message(void)
{
    const char *m = message;

    message = 0;
    return m;
}

int rec_take(int *left, int *right)
{
    if (!tk.ready) {
        return 0;
    }
    tk.ready = 0;
    *left = tk.left;
    *right = tk.right;
    return 1;
}

static void take_begin(void)
{
    const struct rec_setup *rs = &rec_setup;
    uint32_t bytes, per, avail, want;
    uint8_t *base = samples_take_begin(&bytes);

    memset(&tk, 0, sizeof(tk));
    tk.planes = rs->side == RSIDE_LEFT || rs->side == RSIDE_RIGHT ? 1 : 2;
    tk.mode = rs->mode;
    tk.side = rs->side;
    tk.rate = 48000u << cap_rate;
    tk.pre = tk.mode >= RMODE_SOS ? 0 : tk.rate / 100u;
    tk.left = tk.right = -1;
    tk.ev_rd = ev_n;
    per = (bytes / tk.planes) & ~(CACHE_LINE - 1u);
    avail = per / 4u > 2u * SAMPLE_GUARD + 16u ? per / 4u - 2u * SAMPLE_GUARD - 16u : 0;
    want = rs->time ? (uint32_t)rs->time * (tk.rate / 10u) : avail;
    tk.cap = want < avail ? want : avail;
    tk.short_mem = !rs->time || want > avail;
    if (tk.cap < 64u) {
        samples_take_end(0);
        watch = 0;
        state = REC_IDLE;
        message = "SAMPLING MEMORY FULL";
        return;
    }
    tk.base = base;
    tk.p[0] = (int32_t *)base + SAMPLE_GUARD;
    tk.p[1] = tk.planes == 2 ? (int32_t *)(base + plane_bytes(tk.cap)) + SAMPLE_GUARD : tk.p[0];
    memset(base, 0, SAMPLE_GUARD * 4u);
    memset(tk.p[1] - SAMPLE_GUARD, 0, SAMPLE_GUARD * 4u);
    tk.s = trig_at - tk.pre;
    if ((int32_t)(tk.s - cap_start) < 0) {
        tk.s = cap_start;
    }
    if (fw - tk.s > F_SIZE - F_SLACK) {
        tk.s = fw - (F_SIZE - F_SLACK);
    }
    tk.cp = tk.s;
    tk.len = tk.cap;
    state = REC_RECORDING;
}

static int32_t sat_add(int64_t v)
{
    v = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v;
    return (int32_t)v & ~0xff;
}

/* Writes n ring frames from cp at take position i (n doesn't wrap). */
static void put(uint32_t i, uint32_t n, int mix)
{
    int32_t *l = tk.p[0] + i, *r = tk.p[1] + i;
    int c0 = tk.side == RSIDE_RIGHT;

    for (uint32_t k = 0; k < n; k++) {
        const int32_t *f = fring[(tk.cp + k) & F_MASK];
        if (!mix) {
            l[k] = f[c0];
            if (tk.planes == 2) {
                r[k] = f[1];
            }
        } else if (tk.mode == RMODE_SOS) {
            l[k] = sat_add((int64_t)l[k] + f[c0]);
            if (tk.planes == 2) {
                r[k] = sat_add((int64_t)r[k] + f[1]);
            }
        } else {
            l[k] = sat_add(((int64_t)l[k] * 23198 >> 15) + f[c0]);
            if (tk.planes == 2) {
                r[k] = sat_add(((int64_t)r[k] * 23198 >> 15) + f[1]);
            }
        }
    }
    tk.cp += n;
}

static void finish(uint32_t n, uint32_t rot);

/* A stop (SAMPLE or CV) at the current position. */
static void stop_here(void)
{
    if (tk.mode == RMODE_ONCE) {
        finish(tk.wi, 0);
    } else if (tk.mode == RMODE_CIRCULAR) {
        if (tk.total >= tk.cap) {
            finish(tk.cap, tk.total % tk.cap);
        } else {
            finish(tk.total, 0);
        }
    } else if (!tk.pass && !tk.loop_set && tk.wi >= 4u) {
        tk.len = tk.wi;         /* SOS: the first stop sets the loop */
        tk.wi = 0;
        tk.pass = 1;
        tk.loop_set = 1;
    } else if (tk.pass) {
        finish(tk.len, 0);
    }
}

/* Copies ring frames [cp, lim) into the take; 1 when it ended. */
static int copy_to(uint32_t lim)
{
    while (tk.cp != lim && state == REC_RECORDING) {
        uint32_t n = lim - tk.cp;

        if (tk.mode == RMODE_ONCE) {
            n = n < tk.cap - tk.wi ? n : tk.cap - tk.wi;
            put(tk.wi, n, 0);
            tk.wi += n;
            if (tk.wi == tk.cap) {
                if (tk.short_mem) {
                    message = "SAMPLING MEMORY FULL";
                }
                finish(tk.cap, 0);
            }
        } else if (tk.mode == RMODE_CIRCULAR) {
            uint32_t at = tk.total % tk.cap;
            n = n < tk.cap - at ? n : tk.cap - at;
            put(at, n, 0);
            tk.total += n;
        } else {
            n = n < tk.len - tk.wi ? n : tk.len - tk.wi;
            put(tk.wi, n, tk.pass > 0);
            tk.wi += n;
            if (tk.wi == tk.len) {
                tk.wi = 0;
                tk.pass = tk.pass < 255 ? tk.pass + 1 : tk.pass;
            }
        }
    }
    return state != REC_RECORDING;
}

static void take_copy(void)
{
    uint32_t end = fw;

    if (end - tk.cp > F_SIZE - F_SLACK) {
        message = "INPUT OVERRUN";
        tk.ev_rd = ev_n;
        if (state == REC_RECORDING) {
            stop_here();
            if (tk.mode >= RMODE_SOS && state == REC_RECORDING) {
                stop_here();
            }
        }
        return;
    }
    while (state == REC_RECORDING) {
        if (tk.ev_rd != ev_n) {
            uint32_t e = ev_at[tk.ev_rd % EV_MAX];
            if ((int32_t)(e - tk.cp) < 0) {
                e = tk.cp;
            }
            if ((int32_t)(end - e) < 0) {
                copy_to(end);
                return;
            }
            if (copy_to(e)) {
                return;
            }
            tk.ev_rd++;
            stop_here();
            continue;
        }
        copy_to(end);
        return;
    }
}

/* Finishing, in steps: reversals (circular rotation), the right plane moved
 * down to follow a shorter left one, guards, then the samples. */
#define STEP_WORDS (1u << 18)
static struct {
    int32_t *a;         /* reverse a[i..j) */
    uint32_t i, j;
} rev[6];
static int nrev;
static uint32_t mv_done;

static void reverse_add(int32_t *p, uint32_t i, uint32_t j)
{
    if (j - i > 1u) {
        rev[nrev].a = p;
        rev[nrev].i = i;
        rev[nrev].j = j;
        nrev++;
    }
}

static void finish(uint32_t n, uint32_t rot)
{
    watch = 0;
    explicit_arm = 0;
    thr_ok = 0;
    tk.n = n;
    tk.rot = rot;
    nrev = 0;
    mv_done = 0;
    for (int c = 0; rot && c < tk.planes; c++) {
        reverse_add(tk.p[c], 0, rot);
        reverse_add(tk.p[c], rot, tk.cap);
        reverse_add(tk.p[c], 0, tk.cap);
    }
    state = REC_FINISHING;
}

static int names_make(char *l, char *r);

static void commit(void)
{
    char nl[SAMPLE_NAME_LEN + 1], nr[SAMPLE_NAME_LEN + 1];
    uint32_t pb = plane_bytes(tk.n);
    int dual = tk.side == RSIDE_DUAL;

    if (!tk.n) {
        samples_take_end(0);
        message = "NOTHING RECORDED";
        state = REC_IDLE;
        return;
    }
    if (!names_make(nl, dual ? nr : 0)) {
        samples_take_end(0);
        message = "NO FREE SAMPLE NAMES";
        state = REC_IDLE;
        return;
    }
    if (dual) {
        tk.left = samples_add_ram(nl, 1, tk.rate, tk.n, tk.p[0], tk.p[0], 0);
        tk.right = samples_add_ram(nr, 1, tk.rate, tk.n, tk.p[1], tk.p[1], pb);
    } else {
        tk.left = samples_add_ram(nl, tk.planes, tk.rate, tk.n, tk.p[0], tk.p[1], 0);
    }
    if (tk.left < 0 || (dual && tk.right < 0)) {
        samples_take_end(0);
        message = "TOO MANY SAMPLES";
        state = REC_IDLE;
        return;
    }
    samples_take_end(pb * tk.planes);
    tk.ready = 1;
    if (!storage_rec_write(tk.left)) {
        message = storage_error ? storage_error : "NOT SAVED";
        state = REC_IDLE;
        return;
    }
    state = REC_SAVING;
}

static void finish_step(void)
{
    uint32_t budget = STEP_WORDS;

    while (nrev && budget) {
        int32_t *a = rev[0].a;
        uint32_t i = rev[0].i, j = rev[0].j;
        while (i + 1u < j && budget) {
            int32_t t = a[i];
            a[i++] = a[--j];
            a[j] = t;
            budget--;
        }
        rev[0].i = i;
        rev[0].j = j;
        if (i + 1u >= j) {
            nrev--;
            memmove(rev, rev + 1, sizeof(rev[0]) * (unsigned)nrev);
        }
    }
    if (!budget) {
        return;
    }
    if (tk.planes == 2 && tk.n < tk.cap) {
        int32_t *dst = (int32_t *)(tk.base + plane_bytes(tk.n)) + SAMPLE_GUARD;
        uint32_t k = tk.n - mv_done < budget ? tk.n - mv_done : budget;
        for (uint32_t q = 0; q < k; q++) {
            dst[mv_done + q] = tk.p[1][mv_done + q];
        }
        mv_done += k;
        if (mv_done < tk.n) {
            return;
        }
        tk.p[1] = dst;
        memset(dst - SAMPLE_GUARD, 0, SAMPLE_GUARD * 4u);
    }
    memset(tk.p[0] + tk.n, 0, SAMPLE_GUARD * 4u);
    memset(tk.p[1] + tk.n, 0, SAMPLE_GUARD * 4u);
    commit();
}

static int wav_free(const char *name)
{
    return samples_find(name) < 0 && !storage_wav_exists(name);
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Number of a name "<prefix>NNN.wav" or "<prefix>NNN-R.wav", else -1. */
static int name_num(const char *s, const char *pre)
{
    size_t k = 0;
    int n = 0;

    for (; pre[k]; k++) {
        if (lower((unsigned char)s[k]) != lower((unsigned char)pre[k])) {
            return -1;
        }
    }
    for (int d = 0; d < 3; d++, k++) {
        if (s[k] < '0' || s[k] > '9') {
            return -1;
        }
        n = n * 10 + s[k] - '0';
    }
    if (s[k] == '-' && lower((unsigned char)s[k + 1]) == 'r') {
        k += 2;
    }
    return s[k] == '.' && lower((unsigned char)s[k + 1]) == 'w' && lower((unsigned char)s[k + 2]) == 'a' &&
           lower((unsigned char)s[k + 3]) == 'v' && !s[k + 4] ? n : -1;
}

/* New001 / S001 / the preset's first three letters, the number after the
 * highest in the folder. */
static int names_make(char *l, char *r)
{
    char pre[4] = "New";
    int top = 0;

    if (rec_setup.naming == RNAME_S) {
        memcpy(pre, "S", 2);
    } else if (rec_setup.naming == RNAME_PRESET) {
        int k = 0;
        for (int i = 0; preset.name[i] && k < 3; i++) {
            char c = preset.name[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
                pre[k++] = c;
            }
        }
        pre[k] = 0;
        if (!k) {
            memcpy(pre, "New", 4);
        }
    }
    for (int i = 0; i < samples_count(); i++) {
        int n = name_num(samples[i].name, pre);
        top = n > top ? n : top;
    }
    for (int n = top + 1; n <= 999; n++) {
        size_t k = strlen(pre);
        memcpy(l, pre, k);
        l[k] = (char)('0' + n / 100);
        l[k + 1] = (char)('0' + n / 10 % 10);
        l[k + 2] = (char)('0' + n % 10);
        memcpy(l + k + 3, ".wav", 5);
        if (r) {
            memcpy(r, l, k + 3);
            memcpy(r + k + 3, "-R.wav", 7);
        }
        if (wav_free(l) && (!r || wav_free(r))) {
            return 1;
        }
    }
    return 0;
}

static void save_step(void)
{
    int st;

    if (storage_job_active()) {
        return;
    }
    st = storage_job_state();
    storage_job_clear();
    if (st != JOB_DONE) {
        message = storage_error ? storage_error : "NOT SAVED";
        state = REC_IDLE;
        return;
    }
    if (tk.right >= 0 && tk.left >= 0) {
        int r = tk.right;
        tk.left = -1;
        if (!storage_rec_write(r)) {
            message = storage_error ? storage_error : "NOT SAVED";
            state = REC_IDLE;
        }
        return;
    }
    state = REC_IDLE;
}

int rec_save_percent(void)
{
    return state == REC_SAVING ? storage_job_percent() : 0;
}

/* Stereo: each input to its own side. Left, Right and Dual Mono: each
 * input in the centre, at the channel pan law's centre gain (0.75). */
static void monitor_gains(void)
{
    const struct rec_setup *r = &rec_setup;
    float g = r->mon_level > RMON_OFF && !cal_page_open() ? in_gain * db_lin(r->mon_level) : 0.0f;
    float own = r->side == RSIDE_STEREO ? g : 0.75f * g, other = r->side == RSIDE_STEREO ? 0.0f : own;
    float rev = (float)r->mon_rev * 0.01f, dly = (float)r->mon_dly * 0.01f;
    float on[2] = { r->mon_l ? 1.0f : 0.0f, r->mon_r ? 1.0f : 0.0f };

    for (int i = 0; i < 2; i++) {
        mon[i][i] = own * on[i];
        mon[i][1 - i] = other * on[i];
        mon_send[i][0] = g * rev * on[i];
        mon_send[i][1] = g * dly * on[i];
    }
}

int rec_monitor_set(void)
{
    return (rec_setup.mon_l || rec_setup.mon_r) && rec_setup.mon_level > RMON_OFF;
}

int rec_monitor_sends(void)
{
    return (rec_setup.mon_l || rec_setup.mon_r) && rec_setup.mon_level > RMON_OFF &&
           (rec_setup.mon_rev || rec_setup.mon_dly);
}

void rec_poll(void)
{
    static uint8_t last_rate;
    uint32_t t0 = time_us();

    if (rec_setup.rate != last_rate) {
        last_rate = rec_setup.rate;
        cap_reset = 1;
    }
    in_gain = db_lin(rec_setup.level);
    thr_gain = rec_setup.thresh < 0 ? db_lin(rec_setup.thresh) : 2.0f;
    if (state == REC_IDLE && rec_setup.hot && !samples_take_open()) {
        arm(1, 0);
    } else if (state == REC_ARMED && !rec_setup.hot && !explicit_arm) {
        watch = 0;
        state = REC_IDLE;
    }
    cap_want = state != REC_IDLE || metering || rec_setup.mon_l || rec_setup.mon_r;
    monitor_gains();
    {
        uint8_t need = 0;
        for (int c = 0; c < CHANNELS; c++) {
            uint8_t src = preset.ch[c].s.pm_source;
            need |= !preset.ch[c].machine && (src == PM_LEFT || src == PM_RIGHT);
        }
        const struct mix_settings *mx = mix_now();
        need |= mx->delay_tempo == MIX_TEMPO_CV && mx->delay_clk_src >= MIX_CLK_INPUT_L;
        in_need = need;
    }
    if (state == REC_ARMED) {
        if (!trig_new) {
            return;
        }
        trig_new = 0;
        take_begin();
    }
    if (state == REC_RECORDING) {
        take_copy();
    }
    while (state == REC_FINISHING && time_us() - t0 < 4000u) {
        finish_step();
    }
    if (state == REC_SAVING) {
        save_step();
    }
}

void rec_setup_changed(void)
{
    storage_ssetup_changed();
}
