/* Local: upstream's src/eq.S and src/filter.S (the EQ's five direct-form-I
 * sections and the filter's two state-variable sections, eight channels in
 * two four-lane groups) as NEON intrinsics, run a block at a time. Upstream
 * runs every stage of both groups frame by frame; here each group runs the
 * filter, then the EQ section by section, over the whole block. The
 * arithmetic is the same instruction for instruction, so the output is
 * bit-identical; the comments on the method are in eq.S and filter.S's
 * headers (UPSTREAM). */
#include "audio.h"
#include "eq.h"

#include <arm_neon.h>

extern s32 filter_current[2][21][4];

/* filter_current: section 1's a1 a2 a3 k/4, section 2's, the two tap mixes
 * (input, lowpass, bandpass, highpass), section 1's lowpass weight, then the
 * moving cutoff's bases (float G1 K1 G2 K2). filter_state: the sections'
 * ic1 ic2, the wet level, section 1's weighted lowpass. */
enum { CUR_MIX_1 = 8, CUR_MIX_2 = 12, CUR_LP1 = 16, CUR_G1 = 17, CUR_K1, CUR_G2, CUR_K2 };
enum { ST_WET = 4, ST_LP1 = 5 };
#define WET_STEP        0x00aaaaaa
#define GUARD_BITS      5
#define SMOOTH_SHIFT    5
#define EQ_GLIDE_FRAMES 512u

static inline int32x4_t ld(const s32 *p) { return vld1q_s32(p); }
static inline void st(s32 *p, int32x4_t v) { vst1q_s32(p, v); }
static inline int any(int32x4_t v)
{
    uint32x2_t o = vorr_u32(vget_low_u32(vreinterpretq_u32_s32(v)), vget_high_u32(vreinterpretq_u32_s32(v)));
    return (vget_lane_u32(o, 0) | vget_lane_u32(o, 1)) != 0;
}
static inline int same(int32x4_t a, int32x4_t b) { return !any(veorq_s32(a, b)); }
static inline int32x4_t sel(int32x4_t m, int32x4_t a, int32x4_t b)
{
    return vbslq_s32(vreinterpretq_u32_s32(m), a, b);
}

/* ---- filter ---- */

/* One section (filter.S SECTION) on named vectors: input x, coefficients
 * a1 a2 a3 k/4, states ic1 ic2 (masked by run); lowpass, bandpass and
 * highpass out. */
#define SVF(x, a1, a2, a3, kq, ic1, ic2, run, lp, bp, hp)                                              \
    do {                                                                                                \
        int32x4_t v3_ = vqsubq_s32(x, ic2);                                                            \
        bp = vqaddq_s32(vqrdmulhq_s32(a1, ic1), vqrdmulhq_s32(a2, v3_));                               \
        lp = vqaddq_s32(vqaddq_s32(vqrdmulhq_s32(a2, ic1), ic2), vqrdmulhq_s32(a3, v3_));              \
        ic1 = vandq_s32(vqsubq_s32(vqshlq_n_s32(bp, 1), ic1), run);                                    \
        ic2 = vandq_s32(vqsubq_s32(vqshlq_n_s32(lp, 1), ic2), run);                                    \
        hp = vqsubq_s32(vqsubq_s32(x, vqshlq_n_s32(vqrdmulhq_s32(kq, bp), 2)), lp);                    \
    } while (0)
/* A tap mix: input, lowpass, bandpass, highpass. */
#define TAPS(mx, ml, mb, mh, x, lp, bp, hp)                                                             \
    vqaddq_s32(vqaddq_s32(vqrdmulhq_s32(mx, x), vqrdmulhq_s32(ml, lp)),                                 \
               vqaddq_s32(vqrdmulhq_s32(mb, bp), vqrdmulhq_s32(mh, hp)))

/* The current vectors and the published ones they follow. */
static inline u32 published_of(u32 i)
{
    static const u8 at[21] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, FILTER_VECTOR_MIX_2_LP1,
                               FILTER_VECTOR_G1, FILTER_VECTOR_K1, FILTER_VECTOR_G2, FILTER_VECTOR_K2 };
    return at[i];
}
static inline int32x4_t follow(int32x4_t c, const s32 *p) { return vaddq_s32(c, vshrq_n_s32(vsubq_s32(ld(p), c), SMOOTH_SHIFT)); }
static inline float32x4_t ldf(const s32 *p) { return vreinterpretq_f32_s32(ld(p)); }
static inline float32x4_t follow_base(float32x4_t b, const s32 *p)
{
    return vmlaq_f32(b, vsubq_f32(ldf(p), b), vdupq_n_f32(1.0f / 32.0f));
}

/* The moving cutoff (filter.S DYNAMIC_COEFFICIENTS): 2^(the cutoff CV's
 * octaves) as 2^n * (1 + f (c1 + c2 f)), the offset keeping n positive for
 * the truncating convert. The CV holds still through a block. */
static float32x4_t cutoff_cv(const s32 (*pub)[4])
{
    const s32 *input = pub[FILTER_VECTOR_CV_INPUT];
    const u8 *counts = (const u8 *)ta_cv_counts;
    int32x4_t cv = { *(const volatile s16 *)(counts + input[0]), *(const volatile s16 *)(counts + input[1]),
                     *(const volatile s16 *)(counts + input[2]), *(const volatile s16 *)(counts + input[3]) };
    float32x4_t oct = vmulq_f32(vcvtq_f32_s32(cv), ldf(pub[FILTER_VECTOR_CV_SCALE]));
    float32x4_t t = vaddq_f32(oct, vdupq_n_f32(24.0f));
    int32x4_t n = vcvtq_s32_f32(t);
    float32x4_t f = vsubq_f32(t, vcvtq_f32_s32(n));
    float32x4_t m = vmlaq_f32(vdupq_n_f32(0.6565f), vdupq_n_f32(0.3435f), f);
    m = vaddq_f32(vmulq_f32(m, f), vdupq_n_f32(1.0f));
    n = vshlq_n_s32(vaddq_s32(n, vdupq_n_s32(127 - 24)), 23);
    return vmulq_f32(m, vreinterpretq_f32_s32(n));
}

/* A section's coefficients for the lanes whose cutoff moves: g = base * m,
 * then a1 = 1 / (1 + g (g + k)) by a Newton-refined reciprocal, a2 = g a1,
 * a3 = g a2. */
#define MOVE_CUTOFF(base, k, m, dyn, a1, a2, a3)                                                        \
    do {                                                                                                \
        float32x4_t g_ = vminq_f32(vmaxq_f32(vmulq_f32(base, m), vdupq_n_f32(0.0002f)), vdupq_n_f32(16.0f)); \
        float32x4_t d_ = vaddq_f32(vmulq_f32(vaddq_f32(g_, k), g_), vdupq_n_f32(1.0f));               \
        float32x4_t r_ = vrecpeq_f32(d_);                                                              \
        r_ = vmulq_f32(r_, vrecpsq_f32(d_, r_));                                                       \
        r_ = vmulq_f32(r_, vrecpsq_f32(d_, r_));                                                       \
        float32x4_t b_ = vmulq_f32(g_, r_), c_ = vmulq_f32(g_, b_);                                    \
        a1 = sel(dyn, vcvtq_n_s32_f32(r_, 31), a1);                                                    \
        a2 = sel(dyn, vcvtq_n_s32_f32(b_, 31), a2);                                                    \
        a3 = sel(dyn, vcvtq_n_s32_f32(c_, 31), a3);                                                    \
    } while (0)

/* Idle: no state outlives the lanes' time off, and the next lane on starts
 * from the published coefficients. */
static void filter_idle(const s32 (*pub)[4], s32 (*cur)[4], s32 (*state)[4])
{
    for (u32 i = 0; i < 6; ++i) st(state[i], vdupq_n_s32(0));
    for (u32 i = 0; i < 21; ++i) st(cur[i], ld(pub[published_of(i)]));
}

/* A group over the block in two passes, so each section's vectors stay in
 * registers: section 1 into section 2's input and the weighted lowpass,
 * then section 2 and the output (upstream: both, frame by frame). While
 * `moving`, the wet levels fade, (`glide`) the coefficients and mixes close
 * on the published ones and (`dyn`) the cutoff follows its CV, frame by
 * frame as upstream; else every frame runs the same vectors. */
static inline __attribute__((always_inline)) void filter_passes(s32 (*x)[8], u32 g, u32 frames,
    const s32 (*pub)[4], const int moving, const int glide, const int dyn)
{
    s32 (*cur)[4] = filter_current[g], (*state)[4] = filter_state[g];
    const int32x4_t mask = ld(pub[FILTER_VECTOR_MASK]), zero = vdupq_n_s32(0), step = vdupq_n_s32(WET_STEP);
    const int32x4_t dmask = ld(pub[FILTER_VECTOR_DYNAMIC_MASK]);
    const float32x4_t m = dyn ? cutoff_cv(pub) : vdupq_n_f32(0.0f);
    int32x4_t mid[AUDIO_BLOCK], low[AUDIO_BLOCK], wets[AUDIO_BLOCK], runs[AUDIO_BLOCK];
    int32x4_t wet = ld(state[ST_WET]), run = vorrq_s32(vreinterpretq_s32_u32(vcgtq_s32(wet, zero)), mask);
    u32 active = frames;
    const int may_idle = moving && !any(mask);   /* a lane on never idles */
    {
        int32x4_t a1 = ld(cur[0]), a2 = ld(cur[1]), a3 = ld(cur[2]), kq = ld(cur[3]), wl = ld(cur[CUR_LP1]);
        int32x4_t mx = ld(cur[CUR_MIX_1]), ml = ld(cur[CUR_MIX_1 + 1]), mb = ld(cur[CUR_MIX_1 + 2]), mh = ld(cur[CUR_MIX_1 + 3]);
        float32x4_t gb = ldf(cur[CUR_G1]), kb = ldf(cur[CUR_K1]);
        int32x4_t ic1 = ld(state[0]), ic2 = ld(state[1]);
        for (u32 f = 0; f < frames; ++f) {
            if (moving) {
                if (may_idle && !any(wet)) {
                    active = f;
                    break;
                }
                wet = sel(mask, vqaddq_s32(wet, step), vmaxq_s32(vqsubq_s32(wet, step), zero));
                run = vorrq_s32(vreinterpretq_s32_u32(vcgtq_s32(wet, zero)), mask);
                wets[f] = wet;
                runs[f] = run;
            }
            if (glide) {
                a1 = follow(a1, pub[1]);
                a2 = follow(a2, pub[2]);
                a3 = follow(a3, pub[3]);
                kq = follow(kq, pub[4]);
                mx = follow(mx, pub[FILTER_VECTOR_MIX_1]);
                ml = follow(ml, pub[FILTER_VECTOR_MIX_1 + 1]);
                mb = follow(mb, pub[FILTER_VECTOR_MIX_1 + 2]);
                mh = follow(mh, pub[FILTER_VECTOR_MIX_1 + 3]);
                wl = follow(wl, pub[FILTER_VECTOR_MIX_2_LP1]);
                if (dyn) {
                    gb = follow_base(gb, pub[FILTER_VECTOR_G1]);
                    kb = follow_base(kb, pub[FILTER_VECTOR_K1]);
                    MOVE_CUTOFF(gb, kb, m, dmask, a1, a2, a3);
                }
            }
            int32x4_t x1 = vshrq_n_s32(ld(&x[f][4 * g]), GUARD_BITS), lp, bp, hp;
            SVF(x1, a1, a2, a3, kq, ic1, ic2, run, lp, bp, hp);
            low[f] = vqrdmulhq_s32(wl, lp);
            mid[f] = vqshlq_n_s32(TAPS(mx, ml, mb, mh, x1, lp, bp, hp), 1);
        }
        st(cur[0], a1); st(cur[1], a2); st(cur[2], a3); st(cur[3], kq); st(cur[CUR_LP1], wl);
        st(cur[CUR_MIX_1], mx); st(cur[CUR_MIX_1 + 1], ml); st(cur[CUR_MIX_1 + 2], mb); st(cur[CUR_MIX_1 + 3], mh);
        st(cur[CUR_G1], vreinterpretq_s32_f32(dyn ? gb : ldf(pub[FILTER_VECTOR_G1])));
        st(cur[CUR_K1], vreinterpretq_s32_f32(dyn ? kb : ldf(pub[FILTER_VECTOR_K1])));
        st(state[0], ic1);
        st(state[1], ic2);
    }
    {
        int32x4_t a1 = ld(cur[4]), a2 = ld(cur[5]), a3 = ld(cur[6]), kq = ld(cur[7]);
        int32x4_t mx = ld(cur[CUR_MIX_2]), ml = ld(cur[CUR_MIX_2 + 1]), mb = ld(cur[CUR_MIX_2 + 2]), mh = ld(cur[CUR_MIX_2 + 3]);
        float32x4_t gb = ldf(cur[CUR_G2]), kb = ldf(cur[CUR_K2]);
        int32x4_t ic1 = ld(state[2]), ic2 = ld(state[3]), w = wet, r = run;
        for (u32 f = 0; f < active; ++f) {
            if (moving) {
                w = wets[f];
                r = runs[f];
            }
            if (glide) {
                a1 = follow(a1, pub[5]);
                a2 = follow(a2, pub[6]);
                a3 = follow(a3, pub[7]);
                kq = follow(kq, pub[8]);
                mx = follow(mx, pub[FILTER_VECTOR_MIX_2]);
                ml = follow(ml, pub[FILTER_VECTOR_MIX_2 + 1]);
                mb = follow(mb, pub[FILTER_VECTOR_MIX_2 + 2]);
                mh = follow(mh, pub[FILTER_VECTOR_MIX_2 + 3]);
                if (dyn) {
                    gb = follow_base(gb, pub[FILTER_VECTOR_G2]);
                    kb = follow_base(kb, pub[FILTER_VECTOR_K2]);
                    MOVE_CUTOFF(gb, kb, m, dmask, a1, a2, a3);
                }
            }
            int32x4_t in = ld(&x[f][4 * g]), x2 = mid[f], lp, bp, hp;
            SVF(x2, a1, a2, a3, kq, ic1, ic2, r, lp, bp, hp);
            int32x4_t o = vqshlq_n_s32(vqaddq_s32(TAPS(mx, ml, mb, mh, x2, lp, bp, hp), low[f]), GUARD_BITS + 1);
            int32x4_t d = vqaddq_s32(vqrdmulhq_s32(vqsubq_s32(o, in), w), in);
            st(&x[f][4 * g], sel(r, d, in));
        }
        st(cur[4], a1); st(cur[5], a2); st(cur[6], a3); st(cur[7], kq);
        st(cur[CUR_MIX_2], mx); st(cur[CUR_MIX_2 + 1], ml); st(cur[CUR_MIX_2 + 2], mb); st(cur[CUR_MIX_2 + 3], mh);
        st(cur[CUR_G2], vreinterpretq_s32_f32(dyn ? gb : ldf(pub[FILTER_VECTOR_G2])));
        st(cur[CUR_K2], vreinterpretq_s32_f32(dyn ? kb : ldf(pub[FILTER_VECTOR_K2])));
        st(state[2], ic1);
        st(state[3], ic2);
    }
    st(state[ST_WET], wet);
    if (active) st(state[ST_LP1], low[active - 1]);
    if (active < frames) filter_idle(pub, cur, state);
}

static void filter_group(s32 (*x)[8], u32 g, u32 frames, const struct filter_bank *bank)
{
    const s32 (*pub)[4] = bank->vector[g];
    const int32x4_t mask = ld(pub[FILTER_VECTOR_MASK]), wet = ld(filter_state[g][ST_WET]);
    const int32x4_t step = vdupq_n_s32(WET_STEP), zero = vdupq_n_s32(0);
    if (!any(vorrq_s32(mask, wet))) {
        filter_idle(pub, filter_current[g], filter_state[g]);
        return;
    }
    if (pub[FILTER_VECTOR_FLAGS][FILTER_FLAG_DYNAMIC]) {
        filter_passes(x, g, frames, pub, 1, 1, 1);
        return;
    }
    /* Settled: the wet levels at their ends, the coefficients and mixes as
     * close to the published as their glide gets. */
    int fading = !same(sel(mask, vqaddq_s32(wet, step), vmaxq_s32(vqsubq_s32(wet, step), zero)), wet), glide = 0;
    for (u32 i = 0; !glide && i <= CUR_LP1; ++i)
        glide = any(vshrq_n_s32(vsubq_s32(ld(pub[published_of(i)]), ld(filter_current[g][i])), SMOOTH_SHIFT));
    if (glide) filter_passes(x, g, frames, pub, 1, 1, 0);
    else if (fading) filter_passes(x, g, frames, pub, 1, 0, 0);
    else filter_passes(x, g, frames, pub, 0, 0, 0);
}

/* ---- EQ ---- */

/* One direct-form-I section for one frame (eq.S): coefficients c0..c4,
 * history x1 x2 y1 y2 and the saved fraction e, Q4.28. Named vectors, not
 * arrays, so they stay in registers through a block. */
#define BIQUAD(x, y)                                                                                        \
    do {                                                                                                    \
        const int32x4_t x_ = (x);                                                                           \
        int64x2_t lo_ = vmovl_s32(vget_low_s32(e)), hi_ = vmovl_s32(vget_high_s32(e));                     \
        lo_ = vmlal_s32(lo_, vget_low_s32(x_), vget_low_s32(c0));                                            \
        hi_ = vmlal_s32(hi_, vget_high_s32(x_), vget_high_s32(c0));                                          \
        lo_ = vmlal_s32(lo_, vget_low_s32(x1), vget_low_s32(c1));                                           \
        hi_ = vmlal_s32(hi_, vget_high_s32(x1), vget_high_s32(c1));                                         \
        lo_ = vmlal_s32(lo_, vget_low_s32(x2), vget_low_s32(c2));                                           \
        hi_ = vmlal_s32(hi_, vget_high_s32(x2), vget_high_s32(c2));                                         \
        lo_ = vmlal_s32(lo_, vget_low_s32(y1), vget_low_s32(c3));                                           \
        hi_ = vmlal_s32(hi_, vget_high_s32(y1), vget_high_s32(c3));                                         \
        lo_ = vmlal_s32(lo_, vget_low_s32(y2), vget_low_s32(c4));                                           \
        hi_ = vmlal_s32(hi_, vget_high_s32(y2), vget_high_s32(c4));                                         \
        y = vcombine_s32(vqrshrn_n_s64(lo_, 28), vqrshrn_n_s64(hi_, 28));                                   \
        e = vcombine_s32(vmovn_s64(lo_), vmovn_s64(hi_));                                                   \
        e = vshrq_n_s32(vshlq_n_s32(e, 4), 4);                                                              \
        x2 = x1;                                                                                            \
        x1 = x_;                                                                                            \
        y2 = y1;                                                                                            \
        y1 = y;                                                                                             \
    } while (0)

/* A group's five sections over the frames that run: section by section
 * (upstream: frame by frame), the coefficients gliding while `glide` frames
 * of the bank change remain (512 at a publish). */
static void eq_group(s32 (*x)[8], u32 g, u32 frames, u32 glide, const struct eq_group *bank)
{
    struct eq_history_group *fixed = &eq_fixed_data[g];
    int32x4_t w[AUDIO_BLOCK], dry[AUDIO_BLOCK], use[AUDIO_BLOCK];
    u32 run = 0, gliding = glide < frames ? glide : frames;
    int32x4_t m = ld(fixed->mask);
    const int32x4_t target_mask = ld(bank->mask);
    if (!gliding) {
        /* A steady mask: every frame runs, or none. */
        if (!any(m)) return;
        for (u32 f = 0; f < frames; ++f) {
            dry[f] = ld(&x[f][4 * g]);
            w[f] = vshrq_n_s32(dry[f], 3);
        }
        run = frames < 32 ? (1u << frames) - 1u : ~0u;
    } else {
        /* The mask is the union of the old and new while the bank glides,
         * the new from its last frame. A frame runs while the mask, or the
         * mask before it, has a lane: the union until the glide ends, then
         * the new. */
        const int32x4_t both = vorrq_s32(m, target_mask);
        const u32 run_both = any(both) ? ~0u : 0u, run_after = any(target_mask) ? ~0u : 0u;
        for (u32 f = 0; f < frames; ++f) {
            u32 left = glide > f ? glide - f : 0;
            if (left ? run_both : run_after) {
                run |= 1u << f;
                dry[f] = ld(&x[f][4 * g]);
                w[f] = vshrq_n_s32(dry[f], 3);
                use[f] = left > 1 ? both : target_mask;
            }
        }
        st(fixed->mask, gliding == glide ? target_mask : both);
        if (!run) return;
    }
    for (u32 sec = 0; sec < EQ_SECTION_COUNT; ++sec) {
        s32 (*coef)[4] = fixed->section[sec].coefficient, (*hist)[4] = fixed->section[sec].history;
        int32x4_t c0 = ld(coef[0]), c1 = ld(coef[1]), c2 = ld(coef[2]), c3 = ld(coef[3]), c4 = ld(coef[4]);
        int32x4_t x1 = ld(hist[0]), x2 = ld(hist[1]), y1 = ld(hist[2]), y2 = ld(hist[3]), e = ld(hist[4]);
        u32 f = 0;
        if (gliding && (run & 1)) {
            /* The frames that glide run all or none: steps of a 512th of
             * the change (worked out at the publish), the bank's own
             * values at the glide's last frame. */
            const s32 (*target)[4] = bank->coefficient[sec];
            s32 (*steps)[4] = eq_steps[g][sec];
            if (glide == EQ_GLIDE_FRAMES) {
                st(steps[0], vshrq_n_s32(vsubq_s32(ld(target[0]), c0), 9));
                st(steps[1], vshrq_n_s32(vsubq_s32(ld(target[1]), c1), 9));
                st(steps[2], vshrq_n_s32(vsubq_s32(ld(target[2]), c2), 9));
                st(steps[3], vshrq_n_s32(vsubq_s32(ld(target[3]), c3), 9));
                st(steps[4], vshrq_n_s32(vsubq_s32(ld(target[4]), c4), 9));
            }
            const int32x4_t s0 = ld(steps[0]), s1 = ld(steps[1]), s2 = ld(steps[2]), s3 = ld(steps[3]), s4 = ld(steps[4]);
            const u32 stepped = glide <= frames ? glide - 1u : gliding;
            for (; f < stepped; ++f) {
                c0 = vaddq_s32(c0, s0);
                c1 = vaddq_s32(c1, s1);
                c2 = vaddq_s32(c2, s2);
                c3 = vaddq_s32(c3, s3);
                c4 = vaddq_s32(c4, s4);
                BIQUAD(w[f], w[f]);
            }
            if (f < gliding) {
                c0 = ld(target[0]); c1 = ld(target[1]); c2 = ld(target[2]); c3 = ld(target[3]); c4 = ld(target[4]);
                BIQUAD(w[f], w[f]);
                ++f;
            }
        }
        f = f > gliding ? f : gliding;
        for (; f < frames; ++f)
            if (run >> f & 1) BIQUAD(w[f], w[f]);
        st(coef[0], c0); st(coef[1], c1); st(coef[2], c2); st(coef[3], c3); st(coef[4], c4);
        st(hist[0], x1); st(hist[1], x2); st(hist[2], y1); st(hist[3], y2); st(hist[4], e);
    }
    for (u32 f = 0; f < frames; ++f)
        if (run >> f & 1) st(&x[f][4 * g], sel(gliding ? use[f] : m, vqshlq_n_s32(w[f], 3), dry[f]));
}

/* Called from the engine (fw/src/fx.c) once a block: x[frame][channel],
 * updated in place. */
void ta_fx_run(s32 (*x)[8], u32 frames)
{
    /* Nothing publishes while the renderer runs, so a block takes a
     * published bank at its first frame, as upstream does frame by frame. */
    if (filter_mailbox.pending) {
        filter_mailbox.active = filter_mailbox.pending;
        MEMORY_BARRIER();
        filter_mailbox.pending = 0;
    }
    if (eq_mailbox.pending) {
        eq_mailbox.active = eq_mailbox.pending;
        eq_mailbox.remaining = EQ_GLIDE_FRAMES;
        MEMORY_BARRIER();
        eq_mailbox.pending = 0;
    }
    const struct filter_bank *fb = filter_mailbox.active;
    const struct eq_bank *eb = eq_mailbox.active;
    u32 glide = eq_mailbox.remaining;
    for (u32 g = 0; g < 2; ++g) {
        if (fb) filter_group(x, g, frames, fb);
        if (eb) eq_group(x, g, frames, glide, &eb->group[g]);
    }
    if (eb) eq_mailbox.remaining = glide > frames ? glide - frames : 0;
}
