#ifndef INTERP_H_INCLUDED
#define INTERP_H_INCLUDED

#include <stdint.h>

/*
 * Sample interpolation and Aliasing (stock's behaviour, measured): one
 * family of 128-tap polyphase kernels, k = 0..6144, each 33 phase rows
 * (1/32 frame apart, Q15) blended linearly between rows. Positions are
 * Q32.32 frames: the top 5 fraction bits pick the row, the next 27 weigh
 * the blend. Tap 63 reads the frame at the integer position, so a read at
 * q touches x[(q >> 32) - 63 .. (q >> 32) + 64].
 *
 *   k 1024..6144  Kaiser sinc (beta 7, 128 taps), cutoff 2^-n (1 - r/2048)
 *                 of the source Nyquist, n, r = (k - 1024) / 1024, % 1024
 *   k 871..1023   Kaiser sinc at the source Nyquist, window shrinking to ~5 taps
 *   k 664..870    blend of k 871 and linear interpolation (k 663)
 *   k 152..663    linear sharpening to nearest neighbour
 *   k 0..151      nearest neighbour with a gain that zigzags with the phase
 *
 * Kernels whose outer taps are all zero are stored and run short.
 */

#define INTERP_KERNELS  6145
#define INTERP_SHORT    871     /* kernels up to this one read x[(q >> 32) - 3 .. (q >> 32) + 4] */
#define INTERP_REACH_LO 63      /* frames read before the position */
#define INTERP_REACH_HI 64      /* and after */

/* Builds the kernel table (main loop, once; ~45 MB). */
void interp_init(void);

/* Aliasing 0..100 % as stock's signed 16-bit value. */
static inline int interp_alias_x(int pct)
{
    return (int)((float)pct * 655.35f) - 32768;
}

/* Kernel for an Aliasing value x (with its modulation, -32768..32767) and
 * a read step |step| (Q25 source frames per output frame). */
static inline int interp_kernel(int x, uint32_t step)
{
    int nx = -x < 32767 ? -x : 32767;
    int u = 16384 + (nx >> 1);
    int t = u > 24576 ? 4 * (u - 24576) : 0;
    int o = ((u < 24576 ? u : 24576) * 1365 + 16384) >> 15;
    int l = 0;

    t = t < 32767 ? t : 32767;
    if (step >= 1u << 25) {
        int msb = 31 - __builtin_clz(step);
        l = (msb - 25) * 1024 + (int)((step - (1u << msb)) >> (msb - 10));
        l = l < 5119 ? l : 5119;
    }
    return o + ((l * t + 16384) >> 15);
}

/* One output at position q relative to x, kernel k. */
float interp_at(const int32_t *x, int64_t q, int k);

#define INTERP_SLOTS    8
#define INTERP_SPAN_MAX (32 * 64)

/* n outputs at positions q[f], kernels k[f]; slot (one per voice) keeps a
 * float copy of its last kernel. Converting kernels is limited per block
 * (interp_block starts one); a slot over the limit reads the table. */
void interp_block(void);
void interp_run(int slot, const int32_t *x, const int64_t *q, const uint16_t *k, float *out, unsigned n);
/* The same on float samples, kernels k <= INTERP_SHORT only. */
void interp_run_f(int slot, const float *x, const int64_t *q, const uint16_t *k, float *out, unsigned n);

#endif
