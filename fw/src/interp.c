#include "interp.h"
#include "audio.h"

#include <arm_neon.h>

#define TAPS    128
#define ROWS    33
#define CENTRE  63
#define BETA    7.0f
#define K_LIN   663     /* linear interpolation */
#define K_SHORT INTERP_SHORT    /* shortest windowed sinc */
#define K_SINC  1024    /* full-length sinc at the source Nyquist */
#define WMASK   0x7ffffffu
#define KLOAD_BUDGET (2 * ROWS * TAPS / BLOCKS_PER_32)     /* floats converted a block */

/* 2-tap kernels as taps 62..65, the blend as 60..67, the rest as long as
 * their non-zero taps (in 4s). */
#define TAB_MAX ((INTERP_KERNELS - K_SHORT) * ROWS * TAPS + (K_SHORT - K_LIN - 1) * ROWS * 8 + \
                 (K_LIN + 1) * ROWS * 4)

struct kinfo {
    uint32_t off;
    uint8_t lo, n;
};

static int16_t tab[TAB_MAX] __attribute__((aligned(16)));
static struct kinfo kinfo[INTERP_KERNELS];

/* sin(pi x) */
static float sin_pi(float x)
{
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float r = x - (float)n, r2 = r * r;
    float y = r * (3.14159265f + r2 * (-5.16771278f + r2 * (2.55016404f + r2 * (-0.59926453f +
              r2 * (0.08214589f - r2 * 0.00737043f)))));
    return n & 1 ? -y : y;
}

/* copysign(0.5, v); float vector compares would be scalarised by GCC. */
static inline float32x4_t half_as(float32x4_t v)
{
    return vreinterpretq_f32_u32(vorrq_u32(vandq_u32(vreinterpretq_u32_f32(v), vdupq_n_u32(0x80000000u)),
                                           vreinterpretq_u32_f32(vdupq_n_f32(0.5f))));
}

static float32x4_t sin_pi4(float32x4_t x)
{
    int32x4_t n = vcvtq_s32_f32(vaddq_f32(x, half_as(x)));
    float32x4_t r = vsubq_f32(x, vcvtq_f32_s32(n)), r2 = vmulq_f32(r, r);
    float32x4_t p = vmlsq_f32(vdupq_n_f32(0.08214589f), r2, vdupq_n_f32(0.00737043f));
    p = vmlaq_f32(vdupq_n_f32(-0.59926453f), r2, p);
    p = vmlaq_f32(vdupq_n_f32(2.55016404f), r2, p);
    p = vmlaq_f32(vdupq_n_f32(-5.16771278f), r2, p);
    p = vmlaq_f32(vdupq_n_f32(3.14159265f), r2, p);
    p = vmulq_f32(p, r);
    uint32x4_t odd = vtstq_s32(n, vdupq_n_s32(1));
    return vbslq_f32(odd, vnegq_f32(p), p);
}

static float bessel_i0(float x)
{
    float t = 1.0f, sum = 1.0f, q = x * x * 0.25f;

    for (int k = 1; k < 40 && t > 1e-9f * sum; k++) {
        t *= q / (float)(k * k);
        sum += t;
    }
    return sum;
}

static int16_t q15(float v)
{
    v = v * 32768.0f + (v < 0 ? -0.5f : 0.5f);
    return (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)v);
}

/* Stores 33 full rows (scratch) as kernel k, trimmed to its non-zero taps. */
static uint32_t put(int k, const int16_t *rows, int lo, int n, uint32_t off)
{
    if (lo < 0) {
        int a = TAPS, b = 0;
        for (int p = 0; p < ROWS; p++) {
            for (int j = 0; j < TAPS; j++) {
                if (rows[p * TAPS + j]) {
                    a = j < a ? j : a;
                    b = j > b ? j : b;
                }
            }
        }
        lo = a & ~3;
        n = ((b | 3) + 1) - lo;
    }
    kinfo[k] = (struct kinfo){ off, (uint8_t)lo, (uint8_t)n };
    for (int p = 0; p < ROWS; p++) {
        for (int j = 0; j < n; j++) {
            tab[off + (uint32_t)(p * n + j)] = rows[p * TAPS + lo + j];
        }
    }
    return off + (uint32_t)(ROWS * n);
}

/* Two-tap kernels: c[d] for a tap d/32 frame from the read point. */
static void two_tap(int k, float *c)
{
    if (k >= 152) {
        float n = (float)(k - 152) / 16.0f;
        for (int d = 0; d <= 32; d++) {
            float v = n == 0.0f ? (d < 16 ? 1.0f : d == 16 ? 0.5f : 0.0f) : 0.5f + (float)(16 - d) / n;
            c[d] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        }
        return;
    }
    static const int pk[] = { -8, 24, 56, 88, 120, 152 }, pd[] = { 63488, 30720, 14336, 6144, 2048, 0 };
    int s = 0;
    while (k > pk[s + 1]) {
        s++;
    }
    int dd = pd[s] + (pd[s + 1] - pd[s]) * (k - pk[s]) / (pk[s + 1] - pk[s]);
    float g = (float)(32767 - dd) / 32768.0f;
    for (int d = 0; d <= 32; d++) {
        c[d] = d < 16 ? (d & 1 ? 1.0f : g) : d == 16 ? 0.5f : 0.0f;
    }
}

static void two_tap_rows(int k, int16_t *rows)
{
    float c[ROWS];

    two_tap(k, c);
    for (int p = 0; p < ROWS; p++) {
        for (int j = 0; j < TAPS; j++) {
            rows[p * TAPS + j] = 0;
        }
        rows[p * TAPS + CENTRE] = q15(c[p]);
        rows[p * TAPS + CENTRE + 1] = q15(c[32 - p]);
    }
}

static int16x4_t q15_4(float32x4_t v)
{
    v = vmulq_n_f32(v, 32768.0f);
    v = vaddq_f32(v, half_as(v));
    return vqmovn_s32(vcvtq_s32_f32(v));
}

/* sinc(x) I0(beta sqrt(1 - (x/w)^2)) / I0(beta) for |x| < w, else 0. */
static void kaiser_rows(float w, int16_t *rows)
{
    const float iw = 1.0f / w, ni0 = 1.0f / bessel_i0(BETA);
    const float32x4_t one = vdupq_n_f32(1.0f), zero = vdupq_n_f32(0.0f);
    float32x4_t ik2[24];

    for (int k = 0; k < 24; k++) {
        ik2[k] = vdupq_n_f32(1.0f / (float)((k + 1) * (k + 1)));
    }

    for (int p = 0; p < ROWS; p++) {
        for (int j = 0; j < TAPS; j += 4) {
            float32x4_t x = vsubq_f32(vcvtq_f32_s32(vaddq_s32(vdupq_n_s32(j - CENTRE), (int32x4_t){ 0, 1, 2, 3 })),
                                      vdupq_n_f32((float)p / 32.0f));
            float32x4_t u = vmulq_n_f32(x, iw), e = vmlsq_f32(one, u, u);
            uint32x4_t in = vcgtq_s32(vreinterpretq_s32_f32(e), vdupq_n_s32(0));
            uint32x2_t any = vorr_u32(vget_low_u32(in), vget_high_u32(in));
            if (!(vget_lane_u32(any, 0) | vget_lane_u32(any, 1))) {
                vst1_s16(rows + p * TAPS + j, vdup_n_s16(0));
                continue;
            }
            float32x4_t q = vmulq_n_f32(e, BETA * BETA * 0.25f), t = one, i0 = one;
            for (int k = 0; k < 24; k++) {
                t = vmulq_f32(t, vmulq_f32(q, ik2[k]));
                i0 = vaddq_f32(i0, t);
            }
            float32x4_t px = vmulq_n_f32(x, 3.14159265f), r = vrecpeq_f32(px);
            r = vmulq_f32(r, vrecpsq_f32(px, r));
            r = vmulq_f32(r, vrecpsq_f32(px, r));
            uint32x4_t at0 = vceqq_u32(vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x7fffffffu)), vdupq_n_u32(0));
            float32x4_t sn = vbslq_f32(at0, one, vmulq_f32(sin_pi4(x), r));
            float32x4_t h = vmulq_n_f32(vmulq_f32(sn, i0), ni0);
            h = vbslq_f32(in, h, zero);
            vst1_s16(rows + p * TAPS + j, q15_4(h));
        }
    }
}

void interp_init(void)
{
    static int16_t rows[ROWS * TAPS], lin[ROWS * TAPS], shrt[ROWS * TAPS];
    static float a[ROWS * TAPS] __attribute__((aligned(16)));
    static float sc[2 * TAPS] __attribute__((aligned(16)));
    uint32_t off = 0;

    for (int k = 0; k <= K_LIN; k++) {
        two_tap_rows(k, rows);
        off = put(k, rows, CENTRE - 1, 4, off);
    }
    two_tap_rows(K_LIN, lin);
    kaiser_rows(2.0f / (1.0f - (float)(K_SHORT - 863) / 64.0f), shrt);
    for (int k = K_LIN + 1; k < K_SHORT; k++) {
        int m = 157 * (k - K_LIN);
        for (int i = 0; i < ROWS * TAPS; i++) {
            int64_t d = (int64_t)(shrt[i] - lin[i]) * m;
            rows[i] = (int16_t)(lin[i] + (int)((d + 16384) >> 15));
        }
        off = put(k, rows, CENTRE - 3, 8, off);
    }
    for (int k = K_SHORT; k < K_SINC; k++) {
        int n = (k - 863) / 32, r = (k - 863) % 32;
        kaiser_rows((float)(2 << n) / (1.0f - (float)r / 64.0f), rows);
        off = put(k, rows, -1, 0, off);
    }

    /* Full-length kernels: h = w(x) / (pi x) sin(pi fc x), x = m - p/32
     * (m = j - 63), with sin(pi fc x) = S(m) cos(pi fc p/32) - C(m) sin(pi fc p/32). */
    const float i0b = 1.0f / bessel_i0(BETA);
    for (int p = 0; p < ROWS; p++) {
        for (int j = 0; j < TAPS; j++) {
            float x = (float)(j - CENTRE) - (float)p / 32.0f, u = x / 64.0f;
            a[p * TAPS + j] = x == 0.0f || u * u >= 1.0f ? 0.0f :
                              bessel_i0(BETA * __builtin_sqrtf(1.0f - u * u)) * i0b * 32768.0f / (3.14159265f * x);
        }
    }
    for (int k = K_SINC; k < INTERP_KERNELS; k++) {
        int n = (k - K_SINC) / 1024, r = (k - K_SINC) % 1024;
        float fc = (1.0f - (float)r / 2048.0f) / (float)(1 << n);
        float32x4_t vfc = vdupq_n_f32(fc), h = vdupq_n_f32(0.5f);
        for (int j = 0; j < TAPS; j += 4) {
            float32x4_t m = vcvtq_f32_s32(vaddq_s32(vdupq_n_s32(j - CENTRE), (int32x4_t){ 0, 1, 2, 3 }));
            vst1q_f32(sc + j, sin_pi4(vmulq_f32(m, vfc)));
            vst1q_f32(sc + TAPS + j, sin_pi4(vmlaq_f32(h, m, vfc)));
        }
        int16_t *dst = tab + off;
        kinfo[k] = (struct kinfo){ off, 0, TAPS };
        for (int p = 0; p < ROWS; p++) {
            float ph = fc * (float)p / 32.0f;
            const float32x4_t cp = vdupq_n_f32(sin_pi(ph + 0.5f)), sp = vdupq_n_f32(sin_pi(ph));
            for (int j = 0; j < TAPS; j += 4) {
                float32x4_t s = vmulq_f32(vld1q_f32(sc + j), cp);
                s = vmlsq_f32(s, vld1q_f32(sc + TAPS + j), sp);
                s = vmulq_f32(s, vld1q_f32(a + p * TAPS + j));
                vst1_s16(dst + p * TAPS + j, vqmovn_s32(vcvtq_s32_f32(vaddq_f32(s, half_as(s)))));
            }
        }
        dst[CENTRE] = dst[(ROWS - 1) * TAPS + CENTRE + 1] = q15(fc);
        off += ROWS * TAPS;
    }
}

static inline float hsum(float32x4_t a)
{
    float32x2_t h = vadd_f32(vget_low_f32(a), vget_high_f32(a));
    return vget_lane_f32(vpadd_f32(h, h), 0);
}

float interp_at(const int32_t *x, int64_t q, int k)
{
    const struct kinfo *ki = &kinfo[k];
    uint32_t fr = (uint32_t)q;
    const int n = ki->n;
    const int32_t *s = x + (int32_t)(q >> 32) - CENTRE + ki->lo;
    const int16_t *c0 = tab + ki->off + (fr >> 27) * (uint32_t)n, *c1 = c0 + n;
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0;

    for (int j = 0; j < n; j += 4) {
        float32x4_t v = vcvtq_n_f32_s32(vld1q_s32(s + j), 31);
        a0 = vmlaq_f32(a0, v, vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c0 + j)), 15));
        a1 = vmlaq_f32(a1, v, vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c1 + j)), 15));
    }
    float y0 = hsum(a0);
    return y0 + (float)(fr & WMASK) * (1.0f / 134217728.0f) * (hsum(a1) - y0);
}

/* Float copies of each slot's last kernel. */
static float kcache[INTERP_SLOTS][ROWS * TAPS] __attribute__((aligned(64)));
static int16_t kcache_k[INTERP_SLOTS] = { [0 ... INTERP_SLOTS - 1] = -1 };
static unsigned kload_room = KLOAD_BUDGET;

void interp_block(void)
{
    kload_room = KLOAD_BUDGET;
}

/* Float kernel k: the slot's, another slot's copy of k, or 0 if converting
 * it would overrun this block's budget (the caller reads the Q15 rows
 * instead: the same values). Voices sharing a kernel share its lines. */
static const float *kload(int slot, int k)
{
    float *d = kcache[slot];
    unsigned size = ROWS * kinfo[k].n;

    if (kcache_k[slot] != k) {
        for (int i = 0; i < INTERP_SLOTS; i++) {
            if (kcache_k[i] == k) {
                return kcache[i];
            }
        }
        if (size > ROWS * 8 && size > kload_room) {
            return 0;
        }
        kload_room = size < kload_room ? kload_room - size : 0;
        const int16_t *c = tab + kinfo[k].off;
        for (unsigned i = 0; i < size; i += 4) {
            vst1q_f32(d + i, vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c + i)), 15));
        }
        kcache_k[slot] = (int16_t)k;
    }
    return d;
}

static inline float32x4_t q15f(const int16_t *c)
{
    return vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c)), 15);
}

/* One output into *out from float samples s (s[0] is tap lo) and float
 * kernel rows c0, c0 + n (n a multiple of 4, rows 16-byte aligned). Lanes
 * j % 8 < 4 and >= 4 sum apart, then together (the order outputs depend
 * on). Asm: GCC emits 2-register loads without alignment hints here, which
 * leaves the A8 load-bound. */
static inline void dot_f(const float *s, const float *c0, int n, uint32_t fr, float *out)
{
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, b0 = a0, b1 = a0;
    int m = n >> 3;

    if (!(fr & WMASK)) {
        if (m) {
#define DOT1 "vld1.32 {d16-d19}, [%[s]]!\n vld1.32 {d20-d23}, [%[c]:128]!\n" \
             "vmla.f32 %q[a], q8, q10\n vmla.f32 %q[b], q9, q11\n"
            int t;
            __asm__("   lsrs %[t], %[m], #1\n"
                    "   beq 2f\n"
                    "1: " DOT1 DOT1
                    "   subs %[t], %[t], #1\n"
                    "   bne 1b\n"
                    "2: tst %[m], #1\n"
                    "   beq 3f\n"
                    DOT1
                    "3:\n"
                    : [s] "+r"(s), [c] "+r"(c0), [t] "=&r"(t), [a] "+w"(a0), [b] "+w"(b0)
                    : [m] "r"(m)
                    : "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "cc", "memory");
        }
        if (n & 4) {
            a0 = vmlaq_f32(a0, vld1q_f32(s), vld1q_f32(c0));
        }
        a0 = vaddq_f32(a0, b0);
    } else {
        const float *c1 = c0 + n;
        if (m) {
#define DOT2 "vld1.32 {d16-d19}, [%[s]]!\n vld1.32 {d20-d23}, [%[c]:128]!\n vld1.32 {d24-d27}, [%[d]:128]!\n" \
             "vmla.f32 %q[a], q8, q10\n vmla.f32 %q[b], q9, q11\n vmla.f32 %q[e], q8, q12\n vmla.f32 %q[g], q9, q13\n"
            int t;
            __asm__("   lsrs %[t], %[m], #1\n"
                    "   beq 2f\n"
                    "1: " DOT2 DOT2
                    "   subs %[t], %[t], #1\n"
                    "   bne 1b\n"
                    "2: tst %[m], #1\n"
                    "   beq 3f\n"
                    DOT2
                    "3:\n"
                    : [s] "+r"(s), [c] "+r"(c0), [d] "+r"(c1), [t] "=&r"(t), [a] "+w"(a0), [e] "+w"(a1),
                      [b] "+w"(b0), [g] "+w"(b1)
                    : [m] "r"(m)
                    : "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "cc",
                      "memory");
        }
        if (n & 4) {
            float32x4_t v = vld1q_f32(s);
            a0 = vmlaq_f32(a0, v, vld1q_f32(c0));
            a1 = vmlaq_f32(a1, v, vld1q_f32(c1));
        }
        a0 = vaddq_f32(a0, b0);
        a1 = vaddq_f32(a1, b1);
        float32x2_t r = vcvt_n_f32_u32(vdup_n_u32(fr & WMASK), 27);
        a0 = vmlaq_lane_f32(a0, vsubq_f32(a1, a0), r, 0);
    }
    float32x2_t h = vadd_f32(vget_low_f32(a0), vget_high_f32(a0));
    vst1_lane_f32(out, vpadd_f32(h, h), 0);
}

/* dot_f's sums on Q15 rows (a kernel not converted this block). */
static inline void dot_fq(const float *s, const int16_t *c0, int n, uint32_t fr, float *out)
{
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, b0 = a0, b1 = a0;
    const float *e = s + (n & ~7);

    if (!(fr & WMASK)) {
        for (; s < e; s += 8, c0 += 8) {
            a0 = vmlaq_f32(a0, vld1q_f32(s), q15f(c0));
            b0 = vmlaq_f32(b0, vld1q_f32(s + 4), q15f(c0 + 4));
        }
        if (n & 4) {
            a0 = vmlaq_f32(a0, vld1q_f32(s), q15f(c0));
        }
        a0 = vaddq_f32(a0, b0);
    } else {
        const int16_t *c1 = c0 + n;
        for (; s < e; s += 8, c0 += 8, c1 += 8) {
            float32x4_t v = vld1q_f32(s), w = vld1q_f32(s + 4);
            a0 = vmlaq_f32(a0, v, q15f(c0));
            a1 = vmlaq_f32(a1, v, q15f(c1));
            b0 = vmlaq_f32(b0, w, q15f(c0 + 4));
            b1 = vmlaq_f32(b1, w, q15f(c1 + 4));
        }
        if (n & 4) {
            float32x4_t v = vld1q_f32(s);
            a0 = vmlaq_f32(a0, v, q15f(c0));
            a1 = vmlaq_f32(a1, v, q15f(c1));
        }
        a0 = vaddq_f32(a0, b0);
        a1 = vaddq_f32(a1, b1);
        float32x2_t r = vcvt_n_f32_u32(vdup_n_u32(fr & WMASK), 27);
        a0 = vmlaq_lane_f32(a0, vsubq_f32(a1, a0), r, 0);
    }
    float32x2_t h = vadd_f32(vget_low_f32(a0), vget_high_f32(a0));
    vst1_lane_f32(out, vpadd_f32(h, h), 0);
}

/* The same with Q15 kernel rows (one sum per row). */
static inline void dot_q(const float *s, const int16_t *c0, int n, uint32_t fr, float *out)
{
    const int16_t *c1 = c0 + n;
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0;

    for (int j = 0; j < n; j += 4) {
        float32x4_t v = vld1q_f32(s + j);
        a0 = vmlaq_f32(a0, v, q15f(c0 + j));
        a1 = vmlaq_f32(a1, v, q15f(c1 + j));
    }
    float32x2_t r = vcvt_n_f32_u32(vdup_n_u32(fr & WMASK), 27);
    a0 = vmlaq_lane_f32(a0, vsubq_f32(a1, a0), r, 0);
    float32x2_t h = vadd_f32(vget_low_f32(a0), vget_high_f32(a0));
    vst1_lane_f32(out, vpadd_f32(h, h), 0);
}

/* Two Q15 rows of n taps. */
static inline void pld_q15(const int16_t *c, int n)
{
    const char *p = (const char *)c, *e = (const char *)(c + 2 * n);

    for (; p < e; p += 64) {
        __builtin_prefetch(p);
    }
    __builtin_prefetch(e - 1);
}

/* Both rows of a 128-tap kernel (1 KB) into L1 ahead of their frame. */
static inline void pld_rows(const void *p)
{
    const char *c = p;

    __builtin_prefetch(c);
    __builtin_prefetch(c + 64);
    __builtin_prefetch(c + 128);
    __builtin_prefetch(c + 192);
    __builtin_prefetch(c + 256);
    __builtin_prefetch(c + 320);
    __builtin_prefetch(c + 384);
    __builtin_prefetch(c + 448);
    __builtin_prefetch(c + 512);
    __builtin_prefetch(c + 576);
    __builtin_prefetch(c + 640);
    __builtin_prefetch(c + 704);
    __builtin_prefetch(c + 768);
    __builtin_prefetch(c + 832);
    __builtin_prefetch(c + 896);
    __builtin_prefetch(c + 960);
}

void interp_run(int slot, const int32_t *x, const int64_t *q, const uint16_t *k, float *out, unsigned n)
{
    int lo = (int)(q[0] >> 32), hi = lo, same = 1;

    for (unsigned f = 1; f < n; f++) {
        int i = (int)(q[f] >> 32);
        lo = i < lo ? i : lo;
        hi = i > hi ? i : hi;
        same &= k[f] == k[0];
    }
    if (hi - lo > INTERP_SPAN_MAX) {
        for (unsigned f = 0; f < n; f++) {
            out[f] = interp_at(x, q[f], k[f]);
        }
        return;
    }
    float sb[INTERP_SPAN_MAX + TAPS + 4] __attribute__((aligned(64)));
    const int32_t *xs = x + lo - CENTRE;
    const int cnt = hi - lo + TAPS;
    const struct kinfo *ki = &kinfo[k[0]];
    const float *kc = same ? kload(slot, k[0]) : 0;
    const int kn = ki->n;

    if (kc && kn == TAPS) {
        pld_rows(kc + ((uint32_t)q[0] >> 27) * TAPS);
        if (n > 1) {
            pld_rows(kc + ((uint32_t)q[1] >> 27) * TAPS);
        }
    }
    for (int i = 0; i < cnt; i += 16) {
        __builtin_prefetch(sb + i);
    }
    for (int i = 0; i < cnt; i += 4) {
        int j = i < cnt - 4 ? i : cnt - 4;
        vst1q_f32(sb + j, vcvtq_n_f32_s32(vld1q_s32(xs + j), 31));
    }
    if (kc && kn == TAPS) {
        for (unsigned f = 0; f < n; f++) {
            uint32_t fr = (uint32_t)q[f];
            if (f + 2 < n) {
                pld_rows(kc + ((uint32_t)q[f + 2] >> 27) * TAPS);
            }
            dot_f(sb + ((int)(q[f] >> 32) - lo), kc + (fr >> 27) * TAPS, TAPS, fr, out + f);
        }
    } else if (kc) {
        for (unsigned f = 0; f < n; f++) {
            uint32_t fr = (uint32_t)q[f];
            dot_f(sb + ((int)(q[f] >> 32) - lo) + ki->lo, kc + (fr >> 27) * (uint32_t)kn, kn, fr, out + f);
        }
    } else if (same) {
        const int16_t *c = tab + ki->off;
        for (unsigned f = 0; f < n; f++) {
            uint32_t fr = (uint32_t)q[f];
            if (f + 2 < n) {
                pld_q15(c + ((uint32_t)q[f + 2] >> 27) * (uint32_t)kn, kn);
            }
            dot_fq(sb + ((int)(q[f] >> 32) - lo) + ki->lo, c + (fr >> 27) * (uint32_t)kn, kn, fr, out + f);
        }
    } else {
        for (unsigned f = 0; f < n; f++) {
            if (f + 2 < n) {
                const struct kinfo *kp = &kinfo[k[f + 2]];
                pld_q15(tab + kp->off + ((uint32_t)q[f + 2] >> 27) * kp->n, kp->n);
            }
            const struct kinfo *kf = &kinfo[k[f]];
            uint32_t fr = (uint32_t)q[f];
            dot_q(sb + ((int)(q[f] >> 32) - lo) + kf->lo, tab + kf->off + (fr >> 27) * kf->n, kf->n, fr, out + f);
        }
    }
}

void interp_run_f(int slot, const float *x, const int64_t *q, const uint16_t *k, float *out, unsigned n)
{
    int same = 1;

    for (unsigned f = 1; f < n; f++) {
        same &= k[f] == k[0];
    }
    if (!same) {
        for (unsigned f = 0; f < n; f++) {
            const struct kinfo *ki = &kinfo[k[f]];
            uint32_t fr = (uint32_t)q[f];
            dot_q(x + (int)(q[f] >> 32) - CENTRE + ki->lo, tab + ki->off + (fr >> 27) * ki->n, ki->n, fr, out + f);
        }
        return;
    }
    const struct kinfo *ki = &kinfo[k[0]];
    const float *kc = kload(slot, k[0]);
    for (unsigned f = 0; f < n; f++) {
        uint32_t fr = (uint32_t)q[f];
        const float *s = x + (int)(q[f] >> 32) - CENTRE + ki->lo;
        if (kc) {
            dot_f(s, kc + (fr >> 27) * (uint32_t)ki->n, ki->n, fr, out + f);
        } else {
            dot_fq(s, tab + ki->off + (fr >> 27) * (uint32_t)ki->n, ki->n, fr, out + f);
        }
    }
}

#ifdef INTERP_TEST
#include "../tools/interp_test.c"
#endif
