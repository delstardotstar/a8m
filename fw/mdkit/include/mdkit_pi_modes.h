/* Local addition (see ../LOCAL_CHANGES): the PI voices' DSP in NEON, bit-exact;
 * first the eight-mode resonator bank: drum_modes (PI-SD/RS, with
 * block) and <voice>_modal_filter (PI-BD/MT/ML, block 0: a one-sample
 * shorter pipeline, no block or w[0x3f] output, history saved as the
 * saturated word and the raw low word instead of the 48-bit clamp).
 *
 * Each mode's recursion depends only on its own state, its three
 * coefficients (y[0x64 + 4m ..]) and the shared input w[0x60..0x7f]; the
 * modes are coupled only through the saturating carry chain
 * w_{m+1}[i] = sat24(from24(w_m[i]) + state_m[i]). So the recursions run
 * two modes per vector (two vectors a loop), then the chain four samples per vector. The
 * 56-bit accumulators are held as int64 << 8 (wrap mod 2^64 is wrap mod
 * 2^56 above bit 8), so bits 24..55 are the high word and a product is
 * (s24(a) << 8) * (2 * s24(b)); a saturated high word is s24 << 8 too
 * (vqshl leaves 0x7fffffff, so the low byte is cleared). The carry passed to the original is only
 * ever stored where mode 7 overwrites it. */
#ifndef MDKIT_PI_MODES_H
#define MDKIT_PI_MODES_H
#include <stdint.h>
#ifdef __ARM_NEON
#include <arm_neon.h>

/* NEON lanes of 56-bit products: a 24-bit word w as s24(w) << 8 (one
 * shift of the stored word), times 2 s24(k), is the voices' product
 * mpy(w, k) << 8; sums of a few such stay far below 2^63, so the
 * accumulator's high word is acc_to_word's bits 24..55. */
static inline int32x4_t mdkit_pi_w8q(const uint32_t *p)
{
    return vreinterpretq_s32_u32(vshlq_n_u32(vld1q_u32(p), 8));
}

static inline int32_t mdkit_pi_k2(uint32_t w)
{
    return 2 * ((int32_t)(w << 8) >> 8);
}

/* 2 s24(w) of words in memory, loaded straight into NEON (GCC moves
 * scalar multipliers in lane by lane, again at every use): two words, or
 * one in both lanes. */
static inline int32x2_t mdkit_pi_k2v(const uint32_t *p)
{
    return vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(vld1_u32(p), 8)), 7);
}

static inline int32x2_t mdkit_pi_k2d(const uint32_t *p)
{
    return vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(vld1_dup_u32(p), 8)), 7);
}

/* sat24 of the accumulator (held << 8) as s24 << 8; the low byte is 0xff
 * when it saturated high. */
static inline int32x2_t mdkit_pi_sat8(int64x2_t a)
{
    return vqshl_n_s32(vshrn_n_s64(a, 32), 8);
}

/* acc_to_word of four accumulators held << 8. */
static inline uint32x4_t mdkit_pi_words(int64x2_t lo, int64x2_t hi)
{
    return vshrq_n_u32(vreinterpretq_u32_s32(vcombine_s32(mdkit_pi_sat8(lo), mdkit_pi_sat8(hi))), 8);
}
#endif

static inline int32_t mdkit_pi_sat24(int64_t r)
{
    const int32_t v = (int32_t)(uint32_t)((uint64_t)r >> 24);
#ifdef __ARM_FEATURE_SAT
    return __builtin_arm_ssat(v, 24);
#else
    return v < -0x800000 ? -0x800000 : v > 0x7fffff ? 0x7fffff : v;
#endif
}

/* PI-RC/CC/HH metal_filter, bit-exact: R += c0 w[b+i] + 2 c1 w[b+1+i]
 * + c2 x[b+2+i] + c3 x[b+3+i] + c4 x[b+4+i] with w[b+2+i] the saturated
 * output (x the inputs, read before they are overwritten). Accumulators as
 * plain int64: only bits 0..55 are ever looked at. */
static inline void mdkit_pi_metal_filter(uint32_t *work, uint32_t *fraction, unsigned base, uint32_t *history,
                                         uint32_t *frac_out, uint32_t *previous, const uint32_t coeff[5])
{
    int64_t f[32];
    uint32_t *x = work + base;
    const int32_t k0 = 2 * ((int32_t)(coeff[0] << 8) >> 8), k1 = 4 * ((int32_t)(coeff[1] << 8) >> 8);

    for (unsigned i = 0; i < 5; i++) {
        fraction[40 + i] = coeff[i];
    }
#ifdef __ARM_NEON
    /* The feed-forward taps and the outputs/fractions in NEON; only the
     * recursion stays scalar (its accumulators kept for the outputs). */
    int64_t rr[32] __attribute__((aligned(16)));
    const int32x2_t k23 = mdkit_pi_k2v(coeff + 2), k4v = mdkit_pi_k2d(coeff + 4);
    int32x4_t a = vshrq_n_s32(mdkit_pi_w8q(x + 2), 8);
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t n = vshrq_n_s32(mdkit_pi_w8q(x + 6 + i), 8);
        int32x4_t b = vextq_s32(a, n, 1), c = vextq_s32(a, n, 2);
        vst1q_s64(f + i, vmlal_lane_s32(vmlal_lane_s32(vmull_lane_s32(vget_low_s32(a), k23, 0), vget_low_s32(b), k23, 1),
                                        vget_low_s32(c), k4v, 0));
        vst1q_s64(f + i + 2, vmlal_lane_s32(vmlal_lane_s32(vmull_lane_s32(vget_high_s32(a), k23, 0), vget_high_s32(b), k23, 1),
                                            vget_high_s32(c), k4v, 0));
        a = n;
    }
    int64_t r = (int64_t)((uint64_t)*history << 40 | (uint64_t)(*frac_out & 0xffffffu) << 16) >> 16;
    int32_t m2 = (int32_t)(x[0] << 8) >> 8, m1 = (int32_t)(x[1] << 8) >> 8;
    for (unsigned i = 0; i < 32; i += 2) {
        r += f[i];
        r += (int64_t)k0 * m2;
        r += (int64_t)k1 * m1;
        rr[i] = r;
        m2 = mdkit_pi_sat24(r);
        r += f[i + 1];
        r += (int64_t)k0 * m1;
        r += (int64_t)k1 * m2;
        rr[i + 1] = r;
        m1 = mdkit_pi_sat24(r);
    }
    const uint32x4_t low = vdupq_n_u32(0xffffffu);
    for (unsigned i = 0; i < 32; i += 4) {
        int64x2_t r0 = vld1q_s64(rr + i), r1 = vld1q_s64(rr + i + 2);
        int32x4_t hi = vcombine_s32(vshrn_n_s64(r0, 24), vshrn_n_s64(r1, 24));
        int32x4_t s8 = vqshlq_n_s32(hi, 8), c = vshrq_n_s32(s8, 8);
        uint32x4_t lo = vandq_u32(vreinterpretq_u32_s32(vcombine_s32(vmovn_s64(r0), vmovn_s64(r1))), low);
        uint32x4_t alt = vbicq_u32(low, vreinterpretq_u32_s32(vshrq_n_s32(c, 31)));
        vst1q_u32(x + 2 + i, vshrq_n_u32(vreinterpretq_u32_s32(s8), 8));
        vst1q_u32(fraction + base + 2 + i, vbslq_u32(vceqq_s32(c, hi), lo, alt));
    }
#else
    const int32_t k2 = 2 * ((int32_t)(coeff[2] << 8) >> 8), k3 = 2 * ((int32_t)(coeff[3] << 8) >> 8);
    const int32_t k4 = 2 * ((int32_t)(coeff[4] << 8) >> 8);
    for (unsigned i = 0; i < 32; i++) {
        f[i] = (int64_t)k2 * ((int32_t)(x[2 + i] << 8) >> 8) + (int64_t)k3 * ((int32_t)(x[3 + i] << 8) >> 8)
             + (int64_t)k4 * ((int32_t)(x[4 + i] << 8) >> 8);
    }
    int64_t r = (int64_t)((uint64_t)*history << 40 | (uint64_t)(*frac_out & 0xffffffu) << 16) >> 16;
    int32_t m2 = (int32_t)(x[0] << 8) >> 8, m1 = (int32_t)(x[1] << 8) >> 8;
    for (unsigned i = 0; i < 32; i++) {
        r += (int64_t)k0 * m2 + (int64_t)k1 * m1 + f[i];
        int32_t c = mdkit_pi_sat24(r);
        uint32_t lo = c == (int32_t)(uint32_t)((uint64_t)r >> 24) ? (uint32_t)r & 0xffffffu : c < 0 ? 0u : 0xffffffu;
        x[2 + i] = (uint32_t)c & 0xffffffu;
        fraction[base + 2 + i] = lo;
        m2 = m1;
        m1 = c;
    }
#endif
    *history =(uint32_t)m1 & 0xffffffu;
    *frac_out = (uint32_t)r & 0xffffffu;
    *previous = (uint32_t)m2 & 0xffffffu;
}

#ifdef __ARM_NEON
/* PI-RC/CC/HH metal_coupling's mix of a pass (its audio cursor steps
 * 0x100 + 8 pass .. + 7 and never wraps there): out[i] = acc_to_word(
 * mpy(dg, w[0xc9 + i]) + mpy(dg, w[0xc0 + i]) + mpy(eg, w[source + i])). */
static inline void mdkit_pi_metal_mix(uint32_t *out, const uint32_t *work, unsigned source, unsigned machine)
{
    static const uint32_t gains[3][2] = { { 0x733333, 0xccccd }, { 0x7eb852, 0xccccd }, { 0x7eb852, 0x400000 } };
    const int32x2_t k = mdkit_pi_k2v(gains[machine - 70]);
    for (unsigned i = 0; i < 8; i += 4) {
        int32x4_t a = mdkit_pi_w8q(work + 0xc9 + i), b = mdkit_pi_w8q(work + 0xc0 + i);
        int32x4_t c = mdkit_pi_w8q(work + source + i);
        int64x2_t lo = vmlal_lane_s32(vmlal_lane_s32(vmull_lane_s32(vget_low_s32(a), k, 0), vget_low_s32(b), k, 0),
                                      vget_low_s32(c), k, 1);
        int64x2_t hi = vmlal_lane_s32(vmlal_lane_s32(vmull_lane_s32(vget_high_s32(a), k, 0), vget_high_s32(b), k, 0),
                                      vget_high_s32(c), k, 1);
        vst1q_u32(out + i, mdkit_pi_words(lo, hi));
    }
}
#endif

/* PI-RC/CC/HH metal_delay_read/write when every address is in the delay
 * bank (0x130000-0x13ffff; a page of 256 words wraps within itself). */
static inline int mdkit_pi_metal_in_delay(uint32_t a)
{
    a &= 0xffff00u;
    return a >= 0x130000u && a < 0x140000u;
}

static inline void mdkit_pi_metal_delay_read(const uint32_t *delay, uint32_t *work, const uint32_t *gain, uint32_t a,
                                             uint32_t b, uint32_t *history_a, uint32_t *history_b, unsigned out_a,
                                             unsigned out_b)
{
    const uint32_t *pa = delay + ((a & 0xffff00u) - 0x130000u), *pb = delay + ((b & 0xffff00u) - 0x130000u);
#ifdef __ARM_NEON
    const int32x2_t k = mdkit_pi_k2d(gain);
    /* Both one-pole sections as two lanes, accumulators held << 8. */
    uint32_t ta[8], tb[8];
    const uint32_t *xa = pa + (a & 0xff), *xb = pb + (b & 0xff);
    if ((a & 0xff) > 0xf8) {
        for (unsigned i = 0; i < 8; i++) {
            ta[i] = pa[(a + i) & 0xff];
        }
        xa = ta;
    }
    if ((b & 0xff) > 0xf8) {
        for (unsigned i = 0; i < 8; i++) {
            tb[i] = pb[(b + i) & 0xff];
        }
        xb = tb;
    }
    int32x4x2_t z0 = vzipq_s32(mdkit_pi_w8q(xa), mdkit_pi_w8q(xb));
    int32x4x2_t z1 = vzipq_s32(mdkit_pi_w8q(xa + 4), mdkit_pi_w8q(xb + 4));
    uint32x2_t h = vld1_lane_u32(history_b, vld1_lane_u32(history_a, vdup_n_u32(0), 0), 1);
    int64x2_t f = vshll_n_s32(vshl_n_s32(vreinterpret_s32_u32(h), 8), 24);
    const int32x2_t m = vdup_n_s32(-256);
    int32x2_t o0, o1, o2, o3, o4, o5, o6, o7;
#define STEP(o, x) \
    o = vand_s32(mdkit_pi_sat8(f), m); \
    f = vmlsl_s32(vmlal_s32(f, x, k), o, k);
    STEP(o0, vget_low_s32(z0.val[0])) STEP(o1, vget_high_s32(z0.val[0]))
    STEP(o2, vget_low_s32(z0.val[1])) STEP(o3, vget_high_s32(z0.val[1]))
    STEP(o4, vget_low_s32(z1.val[0])) STEP(o5, vget_high_s32(z1.val[0]))
    STEP(o6, vget_low_s32(z1.val[1])) STEP(o7, vget_high_s32(z1.val[1]))
#undef STEP
    int32x4x2_t u0 = vuzpq_s32(vcombine_s32(o0, o1), vcombine_s32(o2, o3));
    int32x4x2_t u1 = vuzpq_s32(vcombine_s32(o4, o5), vcombine_s32(o6, o7));
    vst1q_u32(work + out_a, vshrq_n_u32(vreinterpretq_u32_s32(u0.val[0]), 8));
    vst1q_u32(work + out_a + 4, vshrq_n_u32(vreinterpretq_u32_s32(u1.val[0]), 8));
    vst1q_u32(work + out_b, vshrq_n_u32(vreinterpretq_u32_s32(u0.val[1]), 8));
    vst1q_u32(work + out_b + 4, vshrq_n_u32(vreinterpretq_u32_s32(u1.val[1]), 8));
    h = vshr_n_u32(vreinterpret_u32_s32(mdkit_pi_sat8(f)), 8);
    vst1_lane_u32(history_a, h, 0);
    vst1_lane_u32(history_b, h, 1);
#else
    const int32_t k = 2 * ((int32_t)(*gain << 8) >> 8);
    int64_t fa = (int64_t)((int32_t)(*history_a << 8) >> 8) * (1 << 24);
    int64_t fb = (int64_t)((int32_t)(*history_b << 8) >> 8) * (1 << 24);

    for (unsigned i = 0; i < 8; i++) {
        int32_t oa = mdkit_pi_sat24(fa), ob = mdkit_pi_sat24(fb);
        work[out_a + i] = (uint32_t)oa & 0xffffffu;
        work[out_b + i] = (uint32_t)ob & 0xffffffu;
        fa += (int64_t)k * (((int32_t)(pa[(a + i) & 0xff] << 8) >> 8) - oa);
        fb += (int64_t)k * (((int32_t)(pb[(b + i) & 0xff] << 8) >> 8) - ob);
    }
    *history_a = (uint32_t)mdkit_pi_sat24(fa) & 0xffffffu;
    *history_b = (uint32_t)mdkit_pi_sat24(fb) & 0xffffffu;
#endif
}

static inline void mdkit_pi_metal_delay_write(uint32_t *delay, const uint32_t *work, const uint32_t *gain, uint32_t drive,
                                              unsigned base, unsigned other, unsigned source,
                                              uint32_t *page_a, uint32_t *page_b, int negative)
{
    uint32_t *pa = delay + ((*page_a & 0xffff00u) - 0x130000u), *pb = delay + ((*page_b & 0xffff00u) - 0x130000u);
    const int32_t kd = 2 * ((int32_t)(drive << 8) >> 8);
#ifdef __ARM_NEON
    uint32_t ta[8], tb[8];
    unsigned ia = *page_a & 0xff, ib = *page_b & 0xff;
    uint32_t *da = ia <= 0xf8 ? pa + ia : ta, *db = ib <= 0xf8 ? pb + ib : tb;
    const int32x2_t k = vset_lane_s32(kd, mdkit_pi_k2d(gain), 1), kf = negative ? vneg_s32(k) : k;
    for (unsigned i = 0; i < 8; i += 4) {
        int32x4_t f = mdkit_pi_w8q(work + base + i), o = mdkit_pi_w8q(work + other + i);
        int32x4_t d = mdkit_pi_w8q(work + source + i);
        int64x2_t tl = vmlal_lane_s32(vmull_lane_s32(vget_low_s32(f), kf, 0), vget_low_s32(d), k, 1);
        int64x2_t th = vmlal_lane_s32(vmull_lane_s32(vget_high_s32(f), kf, 0), vget_high_s32(d), k, 1);
        vst1q_u32(db + i, mdkit_pi_words(vmlsl_lane_s32(tl, vget_low_s32(o), k, 0), vmlsl_lane_s32(th, vget_high_s32(o), k, 0)));
        vst1q_u32(da + i, mdkit_pi_words(vmlal_lane_s32(tl, vget_low_s32(o), k, 0), vmlal_lane_s32(th, vget_high_s32(o), k, 0)));
    }
    for (unsigned i = 0; da == ta && i < 8; i++) {
        pa[(ia + i) & 0xff] = ta[i];
    }
    for (unsigned i = 0; db == tb && i < 8; i++) {
        pb[(ib + i) & 0xff] = tb[i];
    }
#else
    const int32_t kg = 2 * ((int32_t)(*gain << 8) >> 8);
    for (unsigned i = 0; i < 8; i++) {
        int64_t f = (int64_t)kg * ((int32_t)(work[base + i] << 8) >> 8);
        int64_t o = (int64_t)kg * ((int32_t)(work[other + i] << 8) >> 8);
        int64_t d = (int64_t)kd * ((int32_t)(work[source + i] << 8) >> 8);
        f = negative ? -f : f;
        pb[(*page_b + i) & 0xff] = (uint32_t)mdkit_pi_sat24(f - o + d) & 0xffffffu;
        pa[(*page_a + i) & 0xff] = (uint32_t)mdkit_pi_sat24(f + o + d) & 0xffffffu;
    }
#endif
    *page_a = (*page_a & 0xffff00u) | ((*page_a + 8) & 0xffu);
    *page_b = (*page_b & 0xffff00u) | ((*page_b + 8) & 0xffu);
}

#ifdef __ARM_NEON
#include <arm_neon.h>

/* The voices' output: on ? s24(block[i]) / 2^23 : 0. A fixed-point
 * conversion with 23 fraction bits is exact for 24-bit values. */
static inline void mdkit_pi_out(float *out, const uint32_t *block, unsigned n, int on)
{
    unsigned i = 0;

    if (on) {
        for (; i + 4 <= n; i += 4) {
            int32x4_t v = vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(vld1q_u32(block + i)), 8), 8);
            vst1q_f32(out + i, vcvtq_n_f32_s32(v, 23));
        }
    }
    for (; i < n; i++) {
        out[i] = on ? (float)((int32_t)(block[i] << 8) >> 8) / 8388608.0f : 0.0f;
    }
}

/* The PI voices' excitation phase ramp: phases phase + i step (i < 32;
 * 56-bit values as int64) as work[i] = mid, fraction[i] = low, and the
 * ROM words at mid and mid + 1 (rom[a - 0x142000]) into x0/x1. Only when
 * every phase stays in 48 bits (to48 never clamps) and every word read
 * lies in the table (a < 0x145000); returns 0 (nothing written) otherwise. */
static inline int mdkit_pi_ramp(uint32_t *work, uint32_t *fraction, int64_t phase, int64_t step, const uint32_t *rom,
                                uint32_t *x0, uint32_t *x1)
{
    const int64_t lim = INT64_C(0x7fffffffffff);
    const int64_t last = phase + 31 * step;
    if (phase > lim || phase < -lim - 1 || last > lim || last < -lim - 1) {
        return 0;
    }
    const uint32_t a0 = (uint32_t)((uint64_t)phase >> 24) & 0xffffffu, a1 = (uint32_t)((uint64_t)last >> 24) & 0xffffffu;
    const uint32_t lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
    if (lo < 0x142000u || hi + 1 >= 0x145000u || (step < 0) != (a1 < a0)) {
        return 0;
    }
    for (unsigned i = 0; i < 32; i++) {
        uint32_t a = (uint32_t)((uint64_t)phase >> 24) & 0xffffffu;
        work[i] = a;
        fraction[i] = (uint32_t)phase & 0xffffffu;
        x0[i] = rom[a - 0x142000u];
        x1[i] = rom[a + 1 - 0x142000u];
        phase += step;
    }
    return 1;
}

/* PI-BD/MT/ML modal_excitation's ramp (phase + step .. + 32 step) and
 * interpolation, under mdkit_pi_ramp's conditions (else 0, nothing
 * written): also fraction[32..63] (the complements' low words) and
 * work[64..95] = acc_to_word(mpy_su(X[w[i]], c[i]) + mpy_su(X[w[i] + 1], f[i])). */
static inline int mdkit_pi_excite(uint32_t *work, uint32_t *fraction, int64_t phase, int64_t step, const uint32_t *rom)
{
    uint32_t x0[32], x1[32];
    if (!mdkit_pi_ramp(work, fraction, phase + step, step, rom, x0, x1)) {
        return 0;
    }
    const uint32x4_t low = vdupq_n_u32(0xffffffu);
    for (unsigned i = 0; i < 32; i += 4) {
        uint32x4_t fr = vld1q_u32(fraction + i), co = vbicq_u32(low, fr);
        vst1q_u32(fraction + 32 + i, co);
        int32x4_t f = vreinterpretq_s32_u32(vshlq_n_u32(fr, 1)), c = vreinterpretq_s32_u32(vshlq_n_u32(co, 1));
        int32x4_t p = mdkit_pi_w8q(x0 + i), q = mdkit_pi_w8q(x1 + i);
        int64x2_t l = vmlal_s32(vmull_s32(vget_low_s32(p), vget_low_s32(c)), vget_low_s32(q), vget_low_s32(f));
        int64x2_t h = vmlal_s32(vmull_s32(vget_high_s32(p), vget_high_s32(c)), vget_high_s32(q), vget_high_s32(f));
        vst1q_u32(work + 64 + i, mdkit_pi_words(l, h));
    }
    return 1;
}

/* PI-SD's split of the ramp's fractions (y) and interpolation, from
 * mdkit_pi_ramp's words x0/x1. The original halves each fraction with an
 * arithmetic shift of the whole accumulator, so the first two (whose
 * accumulators still hold the final phase and phase + step) take bit 24
 * of those as their bit 23 (b0, b1); the rest have clear high words.
 *   y[j] = y[j] >> 1 (| b << 23), y[32 + j] = y[j] - 0x7fffff,
 *   y[64 + i] = acc_to_word(mpy(y[32 + i], x0[i]) - mpy(y[i], x1[i])). */
static inline void mdkit_pi_sd_excite(uint32_t *y, const uint32_t *x0, const uint32_t *x1, uint32_t b0, uint32_t b1)
{
    const uint32_t top[4] = { b0 << 23, b1 << 23, 0, 0 };
    const uint32x4_t low = vdupq_n_u32(0xffffffu), half = vdupq_n_u32(0x7fffffu);
    for (unsigned i = 0; i < 32; i += 4) {
        uint32x4_t v = vshrq_n_u32(vld1q_u32(y + i), 1);
        if (!i) {
            v = vorrq_u32(v, vld1q_u32(top));
        }
        uint32x4_t c = vandq_u32(vsubq_u32(v, half), low);
        vst1q_u32(y + i, v);
        vst1q_u32(y + 32 + i, c);
        int32x4_t p = vreinterpretq_s32_u32(vshlq_n_u32(c, 8)), q = vreinterpretq_s32_u32(vshlq_n_u32(v, 8));
        int32x4_t a = vshrq_n_s32(mdkit_pi_w8q(x0 + i), 7), b = vshrq_n_s32(mdkit_pi_w8q(x1 + i), 7);
        int64x2_t l = vmlsl_s32(vmull_s32(vget_low_s32(p), vget_low_s32(a)), vget_low_s32(q), vget_low_s32(b));
        int64x2_t h = vmlsl_s32(vmull_s32(vget_high_s32(p), vget_high_s32(a)), vget_high_s32(q), vget_high_s32(b));
        vst1q_u32(y + 64 + i, mdkit_pi_words(l, h));
    }
}

/* PI-RS's interpolation from mdkit_pi_ramp's words and fractions (y):
 *   y[64 + i] = acc_to_word(2 from24(x0) - mpy_su(x0, f) + mpy_su(x1, f)). */
static inline void mdkit_pi_rs_excite(uint32_t *y, const uint32_t *x0, const uint32_t *x1)
{
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t f = vreinterpretq_s32_u32(vshlq_n_u32(vld1q_u32(y + i), 1));
        int32x4_t a = mdkit_pi_w8q(x0 + i), b = mdkit_pi_w8q(x1 + i);
        int64x2_t l = vmlal_s32(vmlsl_s32(vshll_n_s32(vget_low_s32(a), 25), vget_low_s32(a), vget_low_s32(f)),
                                vget_low_s32(b), vget_low_s32(f));
        int64x2_t h = vmlal_s32(vmlsl_s32(vshll_n_s32(vget_high_s32(a), 25), vget_high_s32(a), vget_high_s32(f)),
                                vget_high_s32(b), vget_high_s32(f));
        vst1q_u32(y + 64 + i, mdkit_pi_words(l, h));
    }
}

/* Four words x[(pos + i .. + 3) & mask] as s24 << 8 (a ring of
 * mask + 1 words, or mask ~0 for a plain array). */
static inline int32x4_t mdkit_pi_ring8(const uint32_t *x, unsigned pos, unsigned mask)
{
    unsigned k = pos & mask;
    if (((k + 3) & mask) == k + 3) {
        return mdkit_pi_w8q(x + k);
    }
    uint32x4_t v = vld1q_dup_u32(x + k);
    v = vld1q_lane_u32(x + ((pos + 1) & mask), v, 1);
    v = vld1q_lane_u32(x + ((pos + 2) & mask), v, 2);
    v = vld1q_lane_u32(x + ((pos + 3) & mask), v, 3);
    return vreinterpretq_s32_u32(vshlq_n_u32(v, 8));
}

/* Stores four words to x[(pos + i .. + 3) & mask]. */
static inline void mdkit_pi_ring_store(uint32_t *x, unsigned pos, unsigned mask, uint32x4_t v)
{
    unsigned k = pos & mask;
    if (((k + 3) & mask) == k + 3) {
        vst1q_u32(x + k, v);
        return;
    }
    vst1q_lane_u32(x + k, v, 0);
    vst1q_lane_u32(x + ((pos + 1) & mask), v, 1);
    vst1q_lane_u32(x + ((pos + 2) & mask), v, 2);
    vst1q_lane_u32(x + ((pos + 3) & mask), v, 3);
}

/* The voices' one-pole section over a 32-sample block:
 *   out[i] = acc_to_word(s); s = mac(s, x[i], g) - mpy(out[i], g)
 * from s = from24(*state); *state = acc_to_word(s) after. Four of them
 * run as two lane pairs, accumulators held << 8 as in mdkit_pi_sat8.
 * Inputs x[(pos + i) & mask]; out2 (optional) gets out's words too. Each
 * output may overwrite its own input (four samples are loaded first). */
typedef struct {
    const uint32_t *x;
    unsigned pos, mask;
    uint32_t *out, *out2;
    const uint32_t *gain;
    uint32_t *state;
} mdkit_pi_pole;

static inline int64x2_t mdkit_pi_pole_state(const mdkit_pi_pole *a, const mdkit_pi_pole *b)
{
    uint32x2_t h = vld1_lane_u32(b->state, vld1_dup_u32(a->state), 1);
    return vshll_n_s32(vreinterpret_s32_u32(vshl_n_u32(h, 8)), 24);
}

static inline int32x2_t mdkit_pi_pole_gain(const mdkit_pi_pole *a, const mdkit_pi_pole *b)
{
    uint32x2_t g = vld1_lane_u32(b->gain, vld1_dup_u32(a->gain), 1);
    return vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(g, 8)), 7);
}

static inline void mdkit_pi_pole_put(const mdkit_pi_pole *p, unsigned i, uint32x4_t v)
{
    vst1q_u32(p->out + i, v);
    if (p->out2) {
        vst1q_u32(p->out2 + i, v);
    }
}

/* The eight modes' coefficients (PI-BD/MT/ML modal_coefficients, PI-SD/RS
 * drum_coefficients) from gathered words, in NEON:
 *   y[0xb4 + i] = f = acc_to_word(mpy(src[i], pk[i])),
 *   y[0xa0 + i] = c = acc_to_word(from24(0x7fffff) + b + mpy(acc_to_word(mpy(o, 0x155555)), o)),
 *                 b = -mpy(f, f), o = acc_to_word(b),
 * then with d = dm[i] (the damping word) and a = 2 mpy(d, c) - 2^47:
 *   y[0x64 + 4i] = acc_to_word(-mpy(d, d)), y[0x65 + 4i] = acc_to_word(a),
 *   y[0x66 + 4i] = acc_to_word(2 mpy(am[i], acc_to_word(a - mpy(d, d)))). */
static inline void mdkit_pi_coefs(uint32_t *y, const uint32_t *src, const uint32_t *pk, const uint32_t *dm, const uint32_t *am)
{
    static const uint32_t third = 0x155555;
    const int32x2_t k3 = mdkit_pi_k2d(&third);
    const int32x2_t m = vdup_n_s32(-256);
    const int64x2_t one = vdupq_n_s64(INT64_C(0x7fffff) << 32), half = vdupq_n_s64(-(INT64_C(1) << 55));
    for (unsigned h = 0; h < 8; h += 4) {
        int32x4_t s8 = mdkit_pi_w8q(src + h), p2 = vshrq_n_s32(mdkit_pi_w8q(pk + h), 7);
        uint32x4_t fw = mdkit_pi_words(vmull_s32(vget_low_s32(s8), vget_low_s32(p2)), vmull_s32(vget_high_s32(s8), vget_high_s32(p2)));
        vst1q_u32(y + 0xb4 + h, fw);
        int32x4_t f8 = vreinterpretq_s32_u32(vshlq_n_u32(fw, 8)), nf2 = vnegq_s32(vshrq_n_s32(f8, 7));
        int32x4_t d8 = mdkit_pi_w8q(dm + h), nd2 = vnegq_s32(vshrq_n_s32(d8, 7)), a8 = mdkit_pi_w8q(am + h);
        int64x2_t b[2], sq[2], a[2], g[2], c[2];
        for (unsigned j = 0; j < 2; j++) {
            int32x2_t f = j ? vget_high_s32(f8) : vget_low_s32(f8), nf = j ? vget_high_s32(nf2) : vget_low_s32(nf2);
            b[j] = vmull_s32(f, nf);
            int32x2_t o8 = vand_s32(mdkit_pi_sat8(b[j]), m);
            int32x2_t w8 = vand_s32(mdkit_pi_sat8(vmull_lane_s32(o8, k3, 0)), m);
            c[j] = vmlal_s32(vaddq_s64(one, b[j]), w8, vshr_n_s32(o8, 7));
            int32x2_t c4 = vshr_n_s32(vand_s32(mdkit_pi_sat8(c[j]), m), 6);
            int32x2_t d = j ? vget_high_s32(d8) : vget_low_s32(d8), nd = j ? vget_high_s32(nd2) : vget_low_s32(nd2);
            a[j] = vmlal_s32(half, d, c4);
            sq[j] = vmull_s32(d, nd);
            int32x2_t n4 = vshr_n_s32(vand_s32(mdkit_pi_sat8(vaddq_s64(a[j], sq[j])), m), 6);
            g[j] = vmull_s32(j ? vget_high_s32(a8) : vget_low_s32(a8), n4);
        }
        vst1q_u32(y + 0xa0 + h, mdkit_pi_words(c[0], c[1]));
        uint32x4x4_t q = vld4q_u32(y + 0x64 + 4 * h);
        q.val[0] = mdkit_pi_words(sq[0], sq[1]);
        q.val[1] = mdkit_pi_words(a[0], a[1]);
        q.val[2] = mdkit_pi_words(g[0], g[1]);
        vst4q_u32(y + 0x64 + 4 * h, q);
    }
}

/* w[i] = acc_to_word(mpy(g[i], w[i])), i < n (a multiple of 4), g s24 values. */
static inline void mdkit_pi_gain_mul(uint32_t *w, const int32_t *g, unsigned n)
{
    for (unsigned i = 0; i < n; i += 4) {
        int32x4_t k = vshlq_n_s32(vld1q_s32(g + i), 1), x = mdkit_pi_w8q(w + i);
        vst1q_u32(w + i, mdkit_pi_words(vmull_s32(vget_low_s32(x), vget_low_s32(k)), vmull_s32(vget_high_s32(x), vget_high_s32(k))));
    }
}

/* PI-RC/CC/HH metal_output's pairing pass: f[j] = sat24(2 (s24(f[j]) + s24(w[j]))), j < 32. */
static inline void mdkit_pi_metal_double(uint32_t *f, const uint32_t *w)
{
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t sum = vqaddq_s32(mdkit_pi_w8q(f + i), mdkit_pi_w8q(w + i));
        vst1q_u32(f + i, vshrq_n_u32(vreinterpretq_u32_s32(vqshlq_n_s32(sum, 1)), 8));
    }
}

/* PI-RC/CC/HH metal_excitation's noise gate: w[i] = acc_to_word(mpy(n_i,
 * w[i])) with n_0 = f[32], n_i = f[i - 1]. */
static inline void mdkit_pi_metal_noise_mul(uint32_t *w, const uint32_t *f)
{
    int32x4_t n = vextq_s32(vreinterpretq_s32_u32(vld1q_dup_u32(f + 32)), vreinterpretq_s32_u32(vld1q_u32(f)), 3);
    for (unsigned i = 0; i < 32; i += 4) {
        if (i) {
            n = vreinterpretq_s32_u32(vld1q_u32(f + i - 1));
        }
        int32x4_t k = vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(vreinterpretq_u32_s32(n), 8)), 7), x = mdkit_pi_w8q(w + i);
        vst1q_u32(w + i, mdkit_pi_words(vmull_s32(vget_low_s32(x), vget_low_s32(k)), vmull_s32(vget_high_s32(x), vget_high_s32(k))));
    }
}

/* out[i] = acc_to_word(mpy(in[i], *k) << shift), i < 32 (shift <= 5). */
static inline void mdkit_pi_scale(uint32_t *out, const uint32_t *in, const uint32_t *k, int shift)
{
    const int32x2_t g = vshl_s32(mdkit_pi_k2d(k), vdup_n_s32(shift));
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t x = mdkit_pi_w8q(in + i);
        vst1q_u32(out + i, mdkit_pi_words(vmull_s32(vget_low_s32(x), g), vmull_s32(vget_high_s32(x), g)));
    }
}

/* One mdkit_pi_pole section in scalar code: mac(s, x, g) - mpy(o, g) is
 * s + 2 s24(g) (s24(x) - s24(o)), one multiply. */
static inline void mdkit_pi_pole1(const uint32_t *x, uint32_t *out, uint32_t gain, uint32_t *state)
{
    const int32_t k = 2 * ((int32_t)(gain << 8) >> 8);
    int64_t s = (int64_t)((int32_t)(*state << 8) >> 8) * (INT64_C(1) << 24);
    for (unsigned i = 0; i < 32; i++) {
        int32_t o = mdkit_pi_sat24(s);
        out[i] = (uint32_t)o & 0xffffffu;
        s += (int64_t)k * (((int32_t)(x[i] << 8) >> 8) - o);
    }
    *state = (uint32_t)mdkit_pi_sat24(s) & 0xffffffu;
}

/* PI-SD/RS's feedback section: with S_i = s24(w[0xa0+i]) + s24(w[0xc0+i])
 * + s24(w[0x80+i]) and p_i = sat24(S_i) (p_-1 = s24(*prev)), from
 * b = from24(*fb): o_i = acc_to_word(b), b += (S_i - p_{i-1}) 2^24 - o_i k
 * (k = 2 s24 of the gain). o[0] = o_0, o[1 + i] = o_i (s24 values);
 * *fb = acc_to_word(b), *prev = p_31. */
static inline void mdkit_pi_drum_feedback(const uint32_t *w, uint32_t *fb, uint32_t *prev, int32_t k, int32_t o[33])
{
    int64_t d[32] __attribute__((aligned(16)));
    int32x4_t last = vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(vld1q_dup_u32(prev), 8)), 8);
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t sum = vaddq_s32(vaddq_s32(vshrq_n_s32(mdkit_pi_w8q(w + 0xa0 + i), 8), vshrq_n_s32(mdkit_pi_w8q(w + 0xc0 + i), 8)),
                                  vshrq_n_s32(mdkit_pi_w8q(w + 0x80 + i), 8));
        int32x4_t p = vshrq_n_s32(vqshlq_n_s32(sum, 8), 8), dp = vsubq_s32(sum, vextq_s32(last, p, 3));
        vst1q_s64(d + i, vshll_n_s32(vget_low_s32(dp), 24));
        vst1q_s64(d + i + 2, vshll_n_s32(vget_high_s32(dp), 24));
        last = p;
    }
    vst1q_lane_u32(prev, vandq_u32(vreinterpretq_u32_s32(last), vdupq_n_u32(0xffffffu)), 3);
    int64_t b = (int64_t)((int32_t)(*fb << 8) >> 8) * (INT64_C(1) << 24);
    const int32_t nk = -k;
    o[0] = mdkit_pi_sat24(b);
    for (unsigned i = 0; i < 32; i++) {
        int32_t x = mdkit_pi_sat24(b);
        o[1 + i] = x;
        b += d[i];
        b += (int64_t)x * nk;
    }
    *fb = (uint32_t)mdkit_pi_sat24(b) & 0xffffffu;
}

/* PI-BD/MT's three-way mix into the delay rings: with s = mpy(w[0x60+i], *c),
 * k = (w[0xfb], w[0xfc]):
 *   ring[2] <- acc_to_word(s + mpy(k0, w[0x80+i]))
 *   ring[1] <- acc_to_word(s + mpy(k1, w[0xc0+i]) + mpy(k1, w[0xa0+i]))
 *   ring[0] <- acc_to_word(s + mpy(k1, w[0xc0+i]) - mpy(k1, w[0xa0+i]))
 * at ring[j][(pos[j] + i) & 511], i < 32. */
static inline void mdkit_pi_crossmix(const uint32_t *w, const uint32_t *c, uint32_t *const ring[3], const uint32_t pos[3])
{
    const int32x2_t kc = mdkit_pi_k2d(c), k = mdkit_pi_k2v(w + 0xfb);
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t src = mdkit_pi_w8q(w + 0x60 + i), mid = mdkit_pi_w8q(w + 0xa0 + i);
        int32x4_t last = mdkit_pi_w8q(w + 0xc0 + i), first = mdkit_pi_w8q(w + 0x80 + i);
        int64x2_t bl = vmull_lane_s32(vget_low_s32(src), kc, 0), bh = vmull_lane_s32(vget_high_s32(src), kc, 0);
        int64x2_t ll = vmlal_lane_s32(bl, vget_low_s32(last), k, 1), lh = vmlal_lane_s32(bh, vget_high_s32(last), k, 1);
        mdkit_pi_ring_store(ring[2], pos[2] + i, 511,
                            mdkit_pi_words(vmlal_lane_s32(bl, vget_low_s32(first), k, 0),
                                           vmlal_lane_s32(bh, vget_high_s32(first), k, 0)));
        mdkit_pi_ring_store(ring[1], pos[1] + i, 511,
                            mdkit_pi_words(vmlal_lane_s32(ll, vget_low_s32(mid), k, 1),
                                           vmlal_lane_s32(lh, vget_high_s32(mid), k, 1)));
        mdkit_pi_ring_store(ring[0], pos[0] + i, 511,
                            mdkit_pi_words(vmlsl_lane_s32(ll, vget_low_s32(mid), k, 1),
                                           vmlsl_lane_s32(lh, vget_high_s32(mid), k, 1)));
    }
}

/* PI-BD/MT's feedback section. Per sample, with s24 values:
 *   p[i] = sat24(w[0xc0+i] - w[0xa0+i] + w[0x80+i]),
 *   x[i] = acc_to_word(b), b = mpy(0x59999a, x[i]) + 2^23 (p[i] - p[i-1]),
 *   w[0x40+i] = sat24(4 (w[0x40+i] + x[i]));
 * only b's recursion is scalar. *hi is b's word, *prev p's last. */
static inline void mdkit_pi_feedback(uint32_t *w, uint32_t *hi, uint32_t *prev_word)
{
    int64_t d[32] __attribute__((aligned(16)));
    int32_t fb[32];
    int32x4_t last = vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(vld1q_dup_u32(prev_word), 8)), 8);
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t sum = vaddq_s32(vsubq_s32(vshrq_n_s32(mdkit_pi_w8q(w + 0xc0 + i), 8),
                                            vshrq_n_s32(mdkit_pi_w8q(w + 0xa0 + i), 8)),
                                  vshrq_n_s32(mdkit_pi_w8q(w + 0x80 + i), 8));
        int32x4_t p = vshrq_n_s32(vqshlq_n_s32(sum, 8), 8), dp = vsubq_s32(p, vextq_s32(last, p, 3));
        vst1q_s64(d + i, vshll_n_s32(vget_low_s32(dp), 23));
        vst1q_s64(d + i + 2, vshll_n_s32(vget_high_s32(dp), 23));
        last = p;
    }
    vst1q_lane_u32(prev_word, vandq_u32(vreinterpretq_u32_s32(last), vdupq_n_u32(0xffffffu)), 3);
    int64_t acc = (int64_t)((int32_t)(*hi << 8) >> 8) * (INT64_C(1) << 24);
    for (unsigned i = 0; i < 32; ++i) {
        int32_t x = mdkit_pi_sat24(acc);
        fb[i] = x;
        acc = d[i] + (int64_t)x * (2 * 0x59999a);
    }
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t sum = vaddq_s32(vshrq_n_s32(mdkit_pi_w8q(w + 0x40 + i), 8), vld1q_s32(fb + i));
        vst1q_u32(w + 0x40 + i, vshrq_n_u32(vreinterpretq_u32_s32(vqshlq_n_s32(sum, 10)), 8));
    }
    *hi = (uint32_t)mdkit_pi_sat24(acc) & 0xffffffu;
}

static inline void mdkit_pi_poles4(const mdkit_pi_pole p[4])
{
    int64x2_t f01 = mdkit_pi_pole_state(&p[0], &p[1]), f23 = mdkit_pi_pole_state(&p[2], &p[3]);
    const int32x2_t k01 = mdkit_pi_pole_gain(&p[0], &p[1]), k23 = mdkit_pi_pole_gain(&p[2], &p[3]);
    const int32x2_t m = vdup_n_s32(-256);

    for (unsigned i = 0; i < 32; i += 4) {
        int32x4x2_t x01 = vzipq_s32(mdkit_pi_ring8(p[0].x, p[0].pos + i, p[0].mask),
                                    mdkit_pi_ring8(p[1].x, p[1].pos + i, p[1].mask));
        int32x4x2_t x23 = vzipq_s32(mdkit_pi_ring8(p[2].x, p[2].pos + i, p[2].mask),
                                    mdkit_pi_ring8(p[3].x, p[3].pos + i, p[3].mask));
        int32x2_t a0, a1, a2, a3, b0, b1, b2, b3;
#define STEP(a, b, xa, xb) \
        a = vand_s32(mdkit_pi_sat8(f01), m); \
        b = vand_s32(mdkit_pi_sat8(f23), m); \
        f01 = vmlsl_s32(vmlal_s32(f01, xa, k01), a, k01); \
        f23 = vmlsl_s32(vmlal_s32(f23, xb, k23), b, k23);
        STEP(a0, b0, vget_low_s32(x01.val[0]), vget_low_s32(x23.val[0]))
        STEP(a1, b1, vget_high_s32(x01.val[0]), vget_high_s32(x23.val[0]))
        STEP(a2, b2, vget_low_s32(x01.val[1]), vget_low_s32(x23.val[1]))
        STEP(a3, b3, vget_high_s32(x01.val[1]), vget_high_s32(x23.val[1]))
#undef STEP
        int32x4x2_t u01 = vuzpq_s32(vcombine_s32(a0, a1), vcombine_s32(a2, a3));
        int32x4x2_t u23 = vuzpq_s32(vcombine_s32(b0, b1), vcombine_s32(b2, b3));
        mdkit_pi_pole_put(&p[0], i, vshrq_n_u32(vreinterpretq_u32_s32(u01.val[0]), 8));
        mdkit_pi_pole_put(&p[1], i, vshrq_n_u32(vreinterpretq_u32_s32(u01.val[1]), 8));
        mdkit_pi_pole_put(&p[2], i, vshrq_n_u32(vreinterpretq_u32_s32(u23.val[0]), 8));
        mdkit_pi_pole_put(&p[3], i, vshrq_n_u32(vreinterpretq_u32_s32(u23.val[1]), 8));
    }
    uint32x2_t h01 = vshr_n_u32(vreinterpret_u32_s32(mdkit_pi_sat8(f01)), 8);
    uint32x2_t h23 = vshr_n_u32(vreinterpret_u32_s32(mdkit_pi_sat8(f23)), 8);
    vst1_lane_u32(p[0].state, h01, 0);
    vst1_lane_u32(p[1].state, h01, 1);
    vst1_lane_u32(p[2].state, h23, 0);
    vst1_lane_u32(p[3].state, h23, 1);
}

/* One pair of modes (m, m + 1) of mdkit_pi_drum_modes: its lane pair's
 * state, coefficients and previous output. */
typedef struct {
    int64x2_t s;
    int32x2_t prev, k0, k1, k2;
} mdkit_pi_mode_pair;

static inline mdkit_pi_mode_pair mdkit_pi_pair_load(const uint32_t *y, uint32_t (*hist)[3], unsigned m)
{
    mdkit_pi_mode_pair p;
    uint32x2x4_t c = vld4_u32(y + 0x64 + 4 * m);
    p.k0 = vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(c.val[0], 8)), 7);
    p.k1 = vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(c.val[1], 8)), 7);
    p.k2 = vshr_n_s32(vreinterpret_s32_u32(vshl_n_u32(c.val[2], 8)), 7);
    uint32x2_t h0 = vld1_lane_u32(&hist[m + 1][0], vld1_dup_u32(&hist[m][0]), 1);
    uint32x2_t h1 = vld1_lane_u32(&hist[m + 1][1], vld1_dup_u32(&hist[m][1]), 1);
    uint32x2_t h2 = vld1_lane_u32(&hist[m + 1][2], vld1_dup_u32(&hist[m][2]), 1);
    p.s = vaddq_s64(vshll_n_s32(vreinterpret_s32_u32(vshl_n_u32(h0, 8)), 24), vreinterpretq_s64_u64(vshll_n_u32(h1, 8)));
    p.prev = vreinterpret_s32_u32(vshl_n_u32(h2, 8));
    return p;
}

/* hist[m][0] = sat24(state >> 24), [1] its low word (block: of the state
 * clamped to 48 bits), [2] the previous output. */
static inline void mdkit_pi_pair_save(const mdkit_pi_mode_pair *p, uint32_t (*hist)[3], unsigned m, int block)
{
    const uint32x2_t low = vdup_n_u32(0xffffffu);
    int32x2_t h = vshrn_n_s64(p->s, 32), s8 = vqshl_n_s32(h, 8), sat = vshr_n_s32(s8, 8);
    uint32x2_t lo = vand_u32(vreinterpret_u32_s32(vshrn_n_s64(p->s, 8)), low);
    if (block) {
        lo = vbsl_u32(vceq_s32(sat, h), lo, vbic_u32(low, vreinterpret_u32_s32(vshr_n_s32(sat, 31))));
    }
    uint32x2_t h0 = vshr_n_u32(vreinterpret_u32_s32(s8), 8), h2 = vshr_n_u32(vreinterpret_u32_s32(p->prev), 8);
    vst1_lane_u32(&hist[m][0], h0, 0);
    vst1_lane_u32(&hist[m + 1][0], h0, 1);
    vst1_lane_u32(&hist[m][1], lo, 0);
    vst1_lane_u32(&hist[m + 1][1], lo, 1);
    vst1_lane_u32(&hist[m][2], h2, 0);
    vst1_lane_u32(&hist[m + 1][2], h2, 1);
}

/* One sample of a pair: out = the state's high word (bits 24..55) before
 * the step, the input's term (s24 << 8) by lane of x. */
#define MDKIT_PI_PAIR_STEP(p, out, x, lane) \
    do { \
        out = vshrn_n_s64(p.s, 32); \
        int32x2_t old_ = vand_s32(vqshl_n_s32(out, 8), vdup_n_s32(-256)); \
        p.s = vmlal_s32(p.s, p.prev, p.k0); \
        p.s = vmlal_s32(p.s, old_, p.k1); \
        p.s = vmlal_lane_s32(p.s, p.k2, x, lane); \
        p.prev = old_; \
    } while (0)

static inline void mdkit_pi_drum_modes(uint32_t *w, const uint32_t *y, uint32_t (*hist)[3], uint32_t *block)
{
    /* hi[p][i]: the high words of pair p's states after sample i (lanes:
     * its two modes), what the carry chain adds. Two pairs run together. */
    int32_t hi[4][32][2] __attribute__((aligned(16)));
    for (unsigned p = 0; p < 4; p += 2) {
        mdkit_pi_mode_pair a = mdkit_pi_pair_load(y, hist, 2 * p), b = mdkit_pi_pair_load(y, hist, 2 * p + 2);
        int32x2_t ha, hb;
        int32x2_t x = vreinterpret_s32_u32(vshl_n_u32(vld1_u32(w + 0x60), 8));
        MDKIT_PI_PAIR_STEP(a, ha, x, 0);
        MDKIT_PI_PAIR_STEP(b, hb, x, 0);
        MDKIT_PI_PAIR_STEP(a, ha, x, 1);
        MDKIT_PI_PAIR_STEP(b, hb, x, 1);
        vst1_s32(hi[p][0], ha);
        vst1_s32(hi[p + 1][0], hb);
        for (unsigned i = 2; i < 32; i += 2) {
            x = vreinterpret_s32_u32(vshl_n_u32(vld1_u32(w + 0x60 + i), 8));
            MDKIT_PI_PAIR_STEP(a, ha, x, 0);
            MDKIT_PI_PAIR_STEP(b, hb, x, 0);
            vst1_s32(hi[p][i - 1], ha);
            vst1_s32(hi[p + 1][i - 1], hb);
            MDKIT_PI_PAIR_STEP(a, ha, x, 1);
            MDKIT_PI_PAIR_STEP(b, hb, x, 1);
            vst1_s32(hi[p][i], ha);
            vst1_s32(hi[p + 1][i], hb);
        }
        vst1_s32(hi[p][31], vshrn_n_s64(a.s, 32));
        vst1_s32(hi[p + 1][31], vshrn_n_s64(b.s, 32));
        mdkit_pi_pair_save(&a, hist, 2 * p, block != 0);
        mdkit_pi_pair_save(&b, hist, 2 * p + 2, block != 0);
    }
    /* The chain: w_{m+1} = sat24(w_m + hi_m) in 32 bits (from24(w) has a
     * clear low word, so the sum's high word is w + the state's). */
    int32x4_t in7 = vdupq_n_s32(0);
    for (unsigned i = 0; i < 32; i += 4) {
        int32x4_t v = vshrq_n_s32(mdkit_pi_w8q(w + 0x40 + i), 8);
        for (unsigned p = 0; p < 4; p++) {
            int32x4x2_t h = vld2q_s32(hi[p][i]);
            v = vshrq_n_s32(vqshlq_n_s32(vaddq_s32(v, h.val[0]), 8), 8);
            if (p == 3) {
                in7 = v;
            }
            v = vshrq_n_s32(vqshlq_n_s32(vaddq_s32(v, h.val[1]), 8), 8);
        }
        uint32x4_t o = vandq_u32(vreinterpretq_u32_s32(v), vdupq_n_u32(0xffffffu));
        vst1q_u32(w + 0x40 + i, o);
        if (block) {
            vst1q_u32(block + i, o);
        }
    }
    if (block) {
        vst1q_lane_u32(w + 0x3f, vandq_u32(vreinterpretq_u32_s32(in7), vdupq_n_u32(0xffffffu)), 3);
    }
}


#endif
#endif
