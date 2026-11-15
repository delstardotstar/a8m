#ifndef CVFRAME_H_INCLUDED
#define CVFRAME_H_INCLUDED

#include <stdint.h>
#include "audio.h"

/* One ADC's CV A half-band state (chain E in lanes 0-3, L in 4-7). */
struct cv_adc_state {
    int16_t up[8], y[4][8];
};

/* Per-frame CV values (signed 16-bit, 6553.6 per volt) of one ADC's 12
 * inputs from nf scans. r[f]: scan f's raw words (12-bit code << 3) in
 * conversion order per input position 1-4: earlier A, B, later A, C.
 * off, gain: calibration of the ADC's first A input in stock's table order
 * (group g, position k at [g * 8 + k]). out[g][f][k]: group A/B/C. */
void cv_adc_frames(struct cv_adc_state *s, const int16_t *off, const int16_t *gain, const int16_t (*r)[16],
                   unsigned nf, int16_t (*out)[AUDIO_BLOCK][4]);

/* Ring row (word position in an ADC's 16-word scan) of each of the scan
 * positions above. The ADC's sequence is fixed (0-7, 0-3, 8-11); the
 * channel IDs only find where it starts (the pipeline offset), so one bad
 * word cannot move the others. bad: rows whose ID disagrees (or none); a CV
 * A sample in one then uses the input's other sample. */
struct cv_rowmap {
    int8_t row[16];
    uint8_t rot;
    uint16_t bad;
};
void cv_rowmap_init(struct cv_rowmap *m);
/* From one scan's words; 0 (map kept) when too few carry matching IDs. */
int cv_rowmap_update(struct cv_rowmap *m, const uint16_t w[16]);

/* r[f] (f < nf, as cv_adc_frames takes them) from ring rows row[p] (one per
 * scan position, rows of `scans` words, a multiple of 8): frame f takes
 * scan s0 + min(((q0 + f * dq) >> 16) - s0, n - 1), modulo scans. Reads
 * whole 8-scan-aligned groups around s0 .. s0 + n - 1 (n <= CV_RING_SPAN). */
#define CV_RING_SPAN 48
void cv_ring_frames(const uint16_t *const row[16], unsigned scans, unsigned s0, unsigned n, uint32_t q0, uint32_t dq,
                    int16_t (*r)[16], unsigned nf);

#endif
