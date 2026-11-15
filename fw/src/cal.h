#ifndef CAL_H_INCLUDED
#define CAL_H_INCLUDED

#include <stdint.h>
#include <arm_neon.h>

/*
 * Stock's calibration, 0:cal.bin (120 bytes, little-endian): 24 CV offsets
 * and 24 Q15 gains (int16, order 1A..8A, 1B..8B, 1C..8C), sample input L/R
 * DC offsets (int32), mix L/R output offsets (int32, the DAC word gets
 * twice this), channel 1-8 output offsets (int8, x 65536 on the DAC word).
 * Missing or short: stock's defaults. Read once at boot; the Calibrate page
 * edits cal_live and writes the file back (the only boot card writes
 * besides Load Software: cal.bin, calnew.bin, cal.bak).
 */
#define CAL_BYTES       120
#define CAL_CV          24
#define CAL_OFF_NEUTRAL (-16384)
#define CAL_GAIN_DEFAULT (-17920)

struct cal {
    int16_t cv_off[CAL_CV], cv_gain[CAL_CV];
    int32_t in_off[2], mix_off[2];
    int8_t ch_off[8];
};

/* What the audio interrupt uses (cv.c, rec.c, audio.c). */
extern struct cal cal_live;
/* As read from the card at boot or last saved. */
extern struct cal cal_loaded;
extern const char *cal_source;

void cal_load(void);
int cal_dirty(void);
/* cal_live = cal_loaded. */
void cal_discard(void);

/* Writes cal_live to 0:cal.bin (calnew.bin, verified, old one kept as
 * cal.bak); 1 on success (cal_loaded = cal_live). cal_msg says what
 * happened, cal_msg2 the state of the card after a failure. */
int cal_save(void);
extern const char *cal_msg, *cal_msg2;
extern int cal_failed;

/* The Calibrate page: live block means while it is open (CV in stock
 * order, values c = 6553.6 per volt; sample input words, offset applied). */
void cal_page(int on);
/* The input monitor is off while the page is open: its loopback patch
 * (mix out -> sample in) would close a feedback loop through it. */
int cal_page_open(void);
int cal_cv_c(int i);
int32_t cal_in_level(int lr);

/* Capture helpers (stock's Calibrate steps). */
enum {
    CAP_NONE,
    CAP_ZERO_WAIT, CAP_ZERO_MEAS, CAP_ZERO_DONE,
    CAP_GAIN1_WAIT, CAP_GAIN1_MEAS, CAP_GAIN2_WAIT, CAP_GAIN2_MEAS, CAP_GAIN_DONE,
    CAP_OUT_WAIT, CAP_OUT_MEAS, CAP_OUT_DONE,
    CAP_IN_WAIT, CAP_IN_MEAS, CAP_IN_DONE
};
enum { CAPK_ZERO, CAPK_GAIN, CAPK_OUT, CAPK_IN };
#define CAL_SAMPLES 32

/* mask: CV inputs (bit = stock index) for ZERO / GAIN; ignored for OUT
 * (outputs 1-8 = bits 0-7, mix L/R = 8-9) and IN (L/R = bits 0-1). */
void cal_capture(int kind, uint32_t mask);
/* DATA1: measure, or leave a result screen. */
void cal_capture_next(void);
/* Puts back what the capture changed. */
void cal_capture_cancel(void);
int cal_capture_state(void);
uint32_t cal_capture_mask(void);
/* WAIT: ready (ZERO, GAIN2, OUT, IN), +4 V (GAIN1); DONE: updated. */
uint32_t cal_capture_ok(void);
/* GAIN1_WAIT: -4 V seen. */
uint32_t cal_capture_neg(void);
int cal_capture_progress(void);     /* samples taken, 0..CAL_SAMPLES */
/* OUTPUTS measurement: pass number (1..) and an output's best residual. */
int cal_out_pass(void);
int32_t cal_out_resid(int n);
/* Main loop: one sample every CAL_TICK_US while measuring. */
void cal_poll(void);

/* The capture math (host-testable): sums are of CAL_SAMPLES block means
 * times AUDIO_BLOCK (i.e. sums of every frame's c over the samples). */
int cal_zero_offset(int64_t frames_sum, int *valid);
int cal_gain_from(int64_t signed_frames_sum, int *valid);
int cal_ch_offset(int64_t frames_sum, int *valid);
int32_t cal_mix_offset(int64_t level_sum, int *valid);
int32_t cal_in_offset(int64_t level_sum, int *valid);
/* The loopback test's response windows: channel output driven +-127 read
 * as CV A c; mix driven +-MIX_TEST read as the sample input. */
#define CAL_MIX_TEST 0x01480000
/* OUTPUTS: LOOPBACK repeats the one-shot step with the offsets applied,
 * adding each pass's correction (the loop's gain need not be 1) until the
 * residual is small, a correction leaves the stock windows or the total
 * limit (mix +-CAL_MIX_TEST, channel +-66 bytes), or a pass doesn't improve
 * (the best offset is kept). Residuals in sample input words. */
#define CAL_OUT_PASSES 5
struct cal_iter {
    int32_t off, best_off, resid;
    int64_t best_abs;
    int valid, done, started;
};
void cal_iter_init(struct cal_iter *it, int active);
void cal_mix_iter(struct cal_iter *it, int64_t level_sum);
void cal_ch_iter(struct cal_iter *it, int64_t frames_sum);
int cal_ch_responds(int up, int c);
int cal_mix_responds(int up, int32_t reading);

/* The word paths (stock's): sample input reading = offset - raw (the input
 * stage inverts), mix output word = 2 x offset - mix (so does the mix
 * output stage), both saturating; mo = cal_mix_dc(offset). */
static inline int32x4_t cal_in_words(int32x4_t off, int32x4_t raw)
{
    return vqsubq_s32(off, raw);
}
static inline int32x2_t cal_mix_words(int32x2_t mo, int32x2_t mix)
{
    return vqsub_s32(mo, mix);
}
static inline int32_t cal_mix_dc(int32_t off)
{
    int64_t m = 2 * (int64_t)off;
    return (int32_t)(m > INT32_MAX ? INT32_MAX : m < INT32_MIN ? INT32_MIN : m);
}

/* Outputs at 0 (before the offsets) while the loopback step runs. */
extern volatile int cal_mute;

#endif
