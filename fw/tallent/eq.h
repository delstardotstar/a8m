/* Local: upstream's src/eq.h trimmed to the EQ and filter. The stock
 * firmware's addresses, the preset extras, grain, Time Lock, MIDI and panel
 * declarations are gone; struct eq_value and struct filter_value live in
 * fw/src/preset.h (same layout); the hooks at the end are ours (fx.c). */
#ifndef PATCH_EQ_H
#define PATCH_EQ_H
#include "patch.h"
#include "preset.h"
#define EQ_SECTION_COUNT 5u
/* Gain in 0.1 dB. A bell reaches +/-18 dB, the inter-section headroom; a
 * shelf +/-10 dB, where its L1 sum nears the accumulator's sixteen. Every
 * band sweeps the whole audio range; tools/eq_coefficients.py holds the
 * Q4.28, L1 and pole checks for both shapes across it. */
#define EQ_GAIN_MAX 180
#define EQ_SHELF_GAIN_MAX 100
/* A band's q: 0 is its own shape (a shelf for Low and High, the Mid bell
 * whose Q follows its gain); 1..EQ_Q_STEPS is a bell of
 * Q = 0.5 * 2^((q-1)/EQ_Q_STEPS_PER_OCTAVE), 0.5 to 16. */
#define EQ_Q_OWN 0u
#define EQ_Q_STEPS 41u
#define EQ_Q_STEPS_PER_OCTAVE 8
#define EQ_BAND_MID 1u
/* The response graph's range either side of 0 dB. */
#define EQ_GRAPH_DB 21
#define EQ_FREQUENCY_MIN 20u
#define EQ_FREQUENCY_MAX 20000u
/* A band frequency or the filter cutoff is a step of 1/EQ_STEPS_PER_OCTAVE
 * octave, the stored value and the encoder's unit, so a step up and back
 * always returns. 1 kHz is step EQ_STEP_1K, which puts every octave from it
 * (125 Hz, 250 Hz ... 16 kHz) on a step. Step 0 is 20.03 Hz, and the last
 * step is held at EQ_FREQUENCY_MAX. */
#define EQ_STEPS_PER_OCTAVE 120
#define EQ_STEP_1K 677
#define EQ_STEP_MAX 1196
#define EQ_STEP_HZ_1K 1000
#define EQ_STEP_150HZ 349     /* 150.35 Hz, the Low band's default */
#define EQ_STEP_10K 1076      /* 10.02 kHz, the High band's default */
#define EQ_HPF_MIN 20u
#define EQ_HPF_MAX 350u
#define EQ_UNITY 268435456
/* Modulation routes, in CV page order. */
enum { EQ_MOD_LOW_FREQUENCY, EQ_MOD_LOW_GAIN, EQ_MOD_MID_FREQUENCY, EQ_MOD_MID_GAIN,
       EQ_MOD_HIGH_FREQUENCY, EQ_MOD_HIGH_GAIN, EQ_MOD_HPF, EQ_MOD_COUNT };
/* Calibrated CV inputs: 24 signed halfwords, three rows (A/B/C) of eight;
 * 32768 is 5 V. */
#define EQ_CV_INPUTS 24u
#define EQ_CV_SOURCE_OFF -4          /* CV_INPUT_SOURCE_OFF */
#define EQ_CV_SOURCE_RELATIVE_FIRST -3 /* CV A..CV C: the channel's own inputs */
#define EQ_CV_SOURCE_MAX 23          /* 1A..8C */
#define EQ_CV_AMOUNT_MAX 100         /* percent; +1.00 */
/* At +1.00 a volt moves a frequency one octave or a gain 2 dB. */
#define EQ_CV_GAIN_TENTHS_PER_VOLT 20
_Static_assert(EQ_MOD_COUNT == TA_EQ_MODS, "preset.h's eq_value");
enum { FILTER_OFF, FILTER_LP24, FILTER_LP12, FILTER_BP24, FILTER_BP12, FILTER_HP24, FILTER_HP12,
       FILTER_NOTCH24, FILTER_NOTCH12, FILTER_BAND_STOP, FILTER_DUAL, FILTER_TYPES };
enum { FILTER_MOD_CUTOFF, FILTER_MOD_RESONANCE, FILTER_MOD_EXTRA, FILTER_MOD_TYPE, FILTER_MOD_COUNT };
#define FILTER_RESONANCE_MAX 100u
#define FILTER_EXTRA_MAX 100u
/* What a route moves at +1.00 per volt: 5 V takes the cutoff across the
 * whole 20 Hz..20 kHz range, Resonance and Extra across theirs, and Type
 * through every type. */
#define FILTER_CUTOFF_OCTAVES_PER_VOLT 2
#define FILTER_STEPS_PER_VOLT 20
#define FILTER_TYPE_STEPS_PER_VOLT 2
_Static_assert(FILTER_MOD_COUNT == TA_FILTER_MODS, "preset.h's filter_value");
_Static_assert(sizeof(struct filter_value) == 16, "filter values compare by memcmp: no padding");
/* Right-side cutoff: cutoff + right, clamped to 0..EQ_STEP_MAX. */
static inline u32 filter_right_cutoff(const struct filter_value *value)
{
    s32 step = (s32)value->cutoff + value->right;
    return step < 0 ? 0u : step > (s32)EQ_STEP_MAX ? EQ_STEP_MAX : (u32)step;
}
/* What Extra sets for a type, or null when the type has none. */
const char *filter_extra_name(u32 type);
struct eq_group { s32 mask[4], coefficient[5][5][4]; };
struct eq_bank { struct eq_group group[2]; };
struct eq_history_group { s32 mask[4]; struct { s32 coefficient[5][4], history[5][4]; } section[5]; };
extern struct eq_history_group eq_fixed_data[2];
struct eq_mailbox_state { struct eq_bank *active, *pending; u32 remaining; };
extern struct eq_mailbox_state eq_mailbox;
extern s32 eq_steps[2][5][5][4];
void eq_coefficients(const struct eq_value *value, s32 out[5][5]);
/* A band's gain limit for its shape, and every gain brought inside it. */
s32 eq_gain_limit(const struct eq_value *value, u32 band);
void eq_limit(struct eq_value *value);
/* A bell step's Q in hundredths. */
u32 eq_q_hundredths(u32 q);
/* A step's frequency in hundredths of a hertz; the step nearest a frequency
 * in hundredths of a hertz, or a number of cents above EQ_FREQUENCY_MIN; and
 * a step's place, 0..1, across the graphs' 20 Hz..20 kHz log scale. */
u32 eq_step_centihertz(u32 step);
u32 eq_step_nearest(u32 centihertz);
u32 eq_step_above_min(u32 cents);
float eq_frequency_position(float hz);
float eq_step_position(u32 step);
u16 eq_frequency_shift(u16 frequency, float octaves, u16 low, u16 high);
void eq_curve(const struct eq_value *value, u32 points, u8 *out, s32 top, s32 bottom);
void eq_modulate(const struct eq_value *value, u32 channel, struct eq_value *out);
/* A route's CV as the foreground last held it: EQ_CV_COUNTS_FULL is 5 V. A
 * relative source (CV A..CV C) reads `channel`'s inputs; Off reads zero. */
#define EQ_CV_COUNTS_FULL 32768
#define EQ_CENTS_PER_OCTAVE 1200
#define EQ_TENTHS_PER_DB 10
s32 eq_cv_counts(s32 source, u32 channel);
/* The input a source reads for channel, 0..23, or -1 for Off. */
s32 eq_cv_input(s32 source, u32 channel);
u32 eq_modulated(const struct eq_value *value);
void eq_foreground_poll(void);
void eq_changed(void);
void filter_changed(void);
void filter_default(struct filter_value *value, u32 channel);
void filter_foreground_poll(void);
/* One lane of the filter's published vectors (filter.S) for a setting: the
 * lane mask, section 1's a1 a2 a3 and k/4, section 2's, the two tap mixes
 * (input, lowpass, bandpass, highpass each, as Q30 so a weight reaches 2),
 * the lanes whose cutoff its CV moves, with their sections' base g and k
 * (floats), the cutoff CV's input (a byte offset into the calibrated CV
 * block) and octaves per count (float), and the weight of section 1's
 * lowpass in the output, which runs section 2 beside it rather than after
 * it. The last vector holds a group's flags. */
enum { FILTER_VECTOR_MASK, FILTER_VECTOR_SECTION_1, FILTER_VECTOR_SECTION_2 = 5, FILTER_VECTOR_MIX_1 = 9,
       FILTER_VECTOR_MIX_2 = 13, FILTER_VECTOR_DYNAMIC_MASK = 17,
       FILTER_VECTOR_G1, FILTER_VECTOR_K1, FILTER_VECTOR_CV_INPUT, FILTER_VECTOR_CV_SCALE,
       FILTER_VECTOR_G2, FILTER_VECTOR_K2, FILTER_VECTOR_MIX_2_LP1, FILTER_VECTOR_FLAGS, FILTER_VECTORS };
/* A group's flags, one word each: a lane's cutoff moves with its CV. */
enum { FILTER_FLAG_DYNAMIC };
void filter_lane(const struct filter_value *value, s32 out[FILTER_VECTORS]);
void filter_curve(const struct filter_value *value, u32 points, u8 *out, s32 top, s32 bottom, s32 db_top, s32 db_bottom);
/* The setting as channel's routes move it now, cutoff included, for the
 * page's graph; the renderer moves the cutoff itself each block. */
void filter_live(const struct filter_value *value, u32 channel, struct filter_value *out);
u32 filter_modulated(const struct filter_value *value);
void eq_default(struct eq_value *value);

/* The filter's published bank and the renderer's state (eq_model.c). */
struct filter_bank { s32 vector[2][FILTER_VECTORS][4]; } __attribute__((aligned(16)));
struct filter_mailbox_state { struct filter_bank *active, *pending; };
extern struct filter_mailbox_state filter_mailbox;
extern s32 filter_state[2][6][4];

/* Ours (fx.c): the channel whose settings channel plays (its Stereo/Right
 * master), the record it plays now (its own, or its playing zone's), the
 * calibrated CV counts by input (32768 = 5 V, A rows then B, C), and a
 * millisecond clock. */
u32 ta_master(u32 channel);
const struct eq_value *ta_eq_base(u32 master);
const struct filter_value *ta_filter_base(u32 master);
extern int16_t ta_cv_counts[EQ_CV_INPUTS];
u32 ta_ticks(void);
/* Ours: a Stereo/Right channel's offsets on its master's EQ (gains). */
void ta_eq_right(u32 channel, struct eq_value *eq);
/* Renders frames of the eight channels, x[frame][channel] (fx_run.c). */
void ta_fx_run(int32_t (*x)[8], u32 frames);
#endif
