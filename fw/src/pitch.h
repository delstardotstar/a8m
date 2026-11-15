#ifndef PITCH_H_INCLUDED
#define PITCH_H_INCLUDED

#include <arm_neon.h>
#include <stdint.h>

/*
 * Sampler pitch as stock computes it (measured; the exact arithmetic
 * matters at Aliasing 89-100 %, where the zigzag pattern follows the step's
 * fraction at 1/16-frame resolution). A log-pitch P in 2^-26 octave (0 =
 * one source frame per output frame) is built at control rate from the
 * cents and the file's rate; every frame the exponential CVs are added to
 * it, a cubic 2^x (error up to +-0.22 cent) turns it into a step with 25
 * fraction bits, and LinFM is added to the step.
 */

#define PITCH_OCT       (1 << 26)
#define STEP_ONE        (1 << 25)

/* Integer cents as log-pitch (single precision, truncated). */
static inline int32_t pitch_cents(int cents)
{
    return (int32_t)((float)cents * 55924.0546875f);
}

/* A route amount -1.00..+1.00 (hundredths) as Q15. */
static inline int32_t pitch_amount(int v)
{
    return v >= 100 ? 32767 : v >= 0 ? (v * 671089) >> 11 : (int32_t)(((int64_t)v * 1342137) >> 12);
}

/* The file-rate term: a bit-by-bit log2 with 14 fractional bits. */
static inline int32_t pitch_rate(uint32_t rate)
{
    static const float d[15] = { 0.5f, 0.707107f, 0.840896f, 0.917004f, 0.957603f, 0.978572f, 0.989228f,
                                 0.994599f, 0.997296f, 0.998647f, 0.999323f, 0.999662f, 0.999831f, 0.999915f,
                                 0.999958f };

    if (rate == 48000 || rate == 0) {
        return 0;
    }
    if (rate == 96000 || rate == 192000) {
        return rate == 96000 ? PITCH_OCT : 2 * PITCH_OCT;
    }
    float v = (float)rate / 48000.0f, acc = 0.0f, w = 1.0f;
    int neg = v < 1.0f;
    if (neg) {
        v = 1.0f / v;
    }
    for (int i = 0; i < 15;) {
        float t = v * d[i];
        if (t < 1.0f) {
            w *= 0.5f;
            i++;
        } else {
            acc += w;
            v = t;
        }
    }
    int32_t r = (int32_t)(acc * (float)PITCH_OCT);
    return neg ? -r : r;
}

#define PITCH_C1        747121841
#define PITCH_C2        1995136980
#define PITCH_C3        2019842838

/* (E + P) saturated -> step (Q25, 0 .. just under 64). */
static inline int32x4_t pitch_step4(int32x4_t lp)
{
    int32x4_t s = vqaddq_s32(vqaddq_s32(lp, vdupq_n_s32(0x68000000)), vdupq_n_s32((int32_t)0x84000000));
    int32x4_t x = vshlq_n_s32(vandq_s32(s, vdupq_n_s32(PITCH_OCT - 1)), 5);
    int32x4_t y = vqrdmulhq_n_s32(x, PITCH_C1);
    int32x4_t y2 = vqrdmulhq_s32(y, y), y3 = vqrdmulhq_s32(y2, y);
    int32x4_t m = vaddq_s32(vaddq_s32(y, vdupq_n_s32(1 << 30)),
                            vaddq_s32(vqrdmulhq_n_s32(y2, PITCH_C2), vqrdmulhq_n_s32(y3, PITCH_C3)));
    return vrshlq_s32(m, vshrq_n_s32(s, 26));
}

static inline int32_t pitch_step(int32_t lp)
{
    return vgetq_lane_s32(pitch_step4(vdupq_n_s32(lp)), 0);
}

#endif
