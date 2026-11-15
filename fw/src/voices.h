#ifndef VOICES_H_INCLUDED
#define VOICES_H_INCLUDED

#include <stdint.h>
#include "mdkit.h"
#include "preset.h"

/* Machines a channel can run: index 0 is none (the sample channel), 1..30
 * the mdkit voices (TRX, EFM, PI), 31..33 MNMD's GND, 34.. MNMD's
 * Monomachine synths (fw/mnmd). Ids are stored in presets: append only.
 * The largest state (PI-CC/HH/RC) is 270952 bytes. */
#define VOICE_COUNT     44
#define VOICE_STATE_MAX 272384

enum voice_family { FAM_TRX, FAM_EFM, FAM_PI, FAM_GND, FAM_MNM, FAMILY_COUNT };

/* The button page a parameter belongs on. */
enum param_tag { TAG_PITCH, TAG_ENV, TAG_TIMBRE, TAG_MOD, TAG_LEVEL };

enum param_fmt { PF_INT, PF_CHOICE };

/* A choice param is still 0..127: the label is what the MNMD engine makes of
 * that value. q15 selects its quantiser: (v in Q31 >> 16) * n >> 15, else
 * (v in Q31 >> 24) * n >> 7, with Q31 = v * 16909320 (127 = full scale);
 * step, if set, is the engine's own table instead (of v in Q31 >> 24). */
struct param_choice {
    uint8_t n, q15;
    const char *const *labels;
    int (*step)(unsigned v);
};

struct voice_param {
    const char *name;
    int16_t min, max, def;
    uint8_t fmt, tag;
    const struct param_choice *choice;
};

int param_choice_index(const struct param_choice *c, int v);
/* The value at the middle of label `i`'s range. */
int param_choice_value(const struct param_choice *c, int i);

#define VF_PM_IN    1u          /* takes the channel's phase-mod input */
#define VF_LOOP     2u          /* claims LOOP MODE */
#define VF_PITCH_IN 4u          /* plays the channel's pitch itself (set_pitch); only resampled */
#define VF_GATED    8u          /* sustains while held: GATED by default */

struct voice_desc {
    const char *name;           /* also the yml Machine value */
    uint8_t family, nparams, flags;
    uint16_t worst_us;          /* per 667 us block */
    const struct voice_param *params;
    const mdkit_voice *ops;     /* init / set_control / trigger / render */
    float gain;                 /* output trim (tools/levels.sh): default settings peak at 1.0 (a
                                 * normalized sample's full scale), loudest 100 ms RMS at most
                                 * VOICE_RMS_CAP; voice_clip after it */
};

#define VOICE_RMS_CAP 0.5f

/* Soft clipper on a trimmed machine output: linear to 0.5 (-6 dBFS), a
 * quadratic knee to +-1 at 1.5, flat beyond (engine.c machine_trim). */
static inline float voice_clip(float x)
{
    float a = x < 0.0f ? -x : x, d;

    a = a < 1.5f ? a : 1.5f;
    d = a > 0.5f ? a - 0.5f : 0.0f;
    a -= 0.5f * d * d;
    return x < 0.0f ? -a : a;
}

/* VF_PITCH_IN voices' ops: the voice, then its pitch input in Q16 octaves
 * from middle C (0 = 261.63 Hz). */
struct pitched_voice {
    mdkit_voice v;
    void (*set_pitch)(void *state, int32_t octaves);
};

static inline void voice_set_pitch(const struct voice_desc *d, void *state, int32_t octaves)
{
    ((const struct pitched_voice *)d->ops)->set_pitch(state, octaves);
}

struct family_desc {
    const char *name;
    uint8_t count;
    const uint8_t *ids;
};

extern const struct voice_desc *const voices[VOICE_COUNT];
extern const struct family_desc families[FAMILY_COUNT];

/* Absent PlayMode: stock's GATED on sample channels and sustaining
 * machines, 1-SHOT on the others. */
#define PLAY_DEFAULT(machine) \
    ((machine) && !(voices[machine]->flags & VF_GATED) ? PLAY_ONESHOT : PLAY_GATED)

#endif
