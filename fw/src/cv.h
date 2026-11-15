#ifndef CV_H_INCLUDED
#define CV_H_INCLUDED

#include <stdint.h>

#define CV_INPUTS 24    /* 1A, 1B, 1C, 2A, ... 8C */

/* Brings up McSPI0/1 and both MAX11128s (stock's register sequence, plus
 * channel IDs in the results). */
void cv_init(void);
/* Reads one full scan of both ADCs; called from the audio interrupt. */
void cv_poll(void);

/* Input in's voltage at each frame of the block cv_poll last saw (oldest
 * first, AUDIO_BLOCK values), one whole scan per frame, as stock computes
 * it: multiples of 4 / 6553.6 V (DATA2 excepted). Constant when there is no
 * stream. */
const float *cv_frames(int in);

/* Input voltages at the block's last frame, -5..+5 V. */
extern volatile float cv_volts[CV_INPUTS];
/* Raw 12-bit codes by ADC and ADC channel, for the CV monitor. */
extern volatile uint16_t cv_raw[2][16];
/* 1 when results carried channel IDs (else positional mapping is used). */
extern volatile int cv_chan_id_ok;
/* Per ADC: where the scan sequence starts in the 16 words (from the IDs),
 * and the words whose ID disagreed (bit = word). */
extern volatile uint8_t cv_row_rot[2];
extern volatile uint16_t cv_row_bad[2];
/* Bring-up problem (CV then stays at 0 V), or 0; scans that timed out. */
extern const char *cv_warning;
extern volatile uint32_t cv_timeouts;
/* Per-frame CV read position jumps (start, lost blocks, or a read that
 * would have reached unwritten scans). */
extern volatile uint32_t cv_resyncs;
extern volatile uint32_t cv_read_lag;
/* ADC 0's measured scans per audio frame; the furthest the scans fell
 * behind the read servo's expectation after warm-up (scans, negative;
 * about -32 x the rate: a block rendered twice). */
extern volatile float cv_scan_rate;
extern volatile int32_t cv_dev_min;
/* DATA2 standing in for one input: 0 = none, else 1 + input index. */
extern volatile int cv_d2_input;
extern volatile float cv_d2_volts;
/* While cv_sums_on: each input's sum of the last block's values. */
extern volatile int cv_sums_on;
extern volatile int32_t cv_sums[CV_INPUTS];
extern volatile uint32_t cv_sums_epoch;

#endif
