#ifndef ENGINE_H_INCLUDED
#define ENGINE_H_INCLUDED

#include <stdint.h>
#include "audio.h"
#include "preset.h"

/* The audio renderer: 8 channels of mdkit voices driven by the live
 * preset, gates and CV. Runs in the audio interrupt (audio_start). */
void engine_render(int32_t out[][AUDIO_OUTPUTS], int32_t mix[][2],
                   const int32_t in[][2], unsigned frames);

/* Prepares voices for machine changes outside the IRQ; call from the main
 * loop (also once before audio_start). */
void engine_service(void);

/* Trigger a channel from the UI (as if its gate rose). */
void engine_trigger(int ch);
/* Trigger On Select: fires the live preset's AutoTrigger channels (as their
 * buttons would, held back until a new machine is ready). */
void engine_auto_trigger(void);
/* Latch (PLAY MODE + CHn): toggles holding ch's gate high. Latching fires
 * it like its button; while latched, gates reaching it don't re-attack;
 * unlatching fades out whatever the latch fired (a manual stop). */
void engine_latch(int ch);
/* Gate Delay: gate-input triggers start this long after the edge is seen,
 * sample-accurately; 0.1 ms units, 0..50. Buttons are not delayed. */
void engine_set_gate_delay(int tenths_ms);

/* Zone Lock: a sample channel plays zone n - 1 whatever selects it, 0 = off. */
extern volatile uint8_t engine_zone_lock[CHANNELS];
/* Sample voices: stopped before the sample pool is reused (folder load). */
void engine_samples_stop(void);
/* A clean start (preset load): engine_hush fades every output out (~5 ms),
 * stops every voice (machines, samples, grains, Time Lock) and returns
 * once the renderer holds silence; triggers meanwhile wait. engine_unhush
 * renders again, the waiting triggers first. Main loop only. */
void engine_hush(void);
void engine_unhush(void);
/* Channel buttons held (bit per channel), from the main loop: a gated
 * sample fired by a button plays while it is held. */
extern volatile uint8_t engine_buttons;
/* Channels held down by the UI (auditions): ORed with engine_buttons. */
extern volatile uint8_t engine_virtual_buttons;

/* Inc/Dec/Reset preset by CV: inputs (0 = off, else 1 + 1A..8C index).
 * A rise to >= 1.7 V after <= 1.5 V counts in engine_status.preset_cv. */
enum { PCV_INC, PCV_DEC, PCV_RESET, PCV_COUNT };
extern volatile uint8_t engine_preset_cv_in[PCV_COUNT];

struct engine_status {
    float peak[CHANNELS];       /* per channel output, 0..1, decaying */
    float mix_peak[2];
    float in_peak[2];
    uint8_t awake;              /* bit per channel rendering */
    uint8_t gates;
    uint32_t render_us_max, render_us_last;
    uint32_t overloads;         /* times all voices were put to sleep */
    uint32_t triggers[CHANNELS];    /* count per channel, for the LEDs */
    /* What last fired each channel: 0-7 = that channel's gate input,
     * 8-15 = that channel's button, 16-23 = that channel's latch (for LEDs
     * that stay lit while held), TRIG_SRC_AUTO = Trigger On Select. */
    uint8_t trigger_src[CHANNELS];
    uint8_t latched;
    uint8_t zone[CHANNELS];             /* sample voice's zone, last played */
    uint32_t preset_cv[PCV_COUNT];      /* counted rises */
    uint32_t preset_cv_ticks;           /* time_ticks of the last one */
};
#define TRIG_SRC_LATCH  16
#define TRIG_SRC_AUTO   24
extern volatile struct engine_status engine_status;

/* Waveform of one channel for the home screen: min/max per 16 frames,
 * a ring of SCOPE_N buckets, newest at scope_head - 1. */
#define SCOPE_N 56
extern volatile int engine_scope_ch;
extern volatile int8_t engine_scope_min[SCOPE_N], engine_scope_max[SCOPE_N];
extern volatile unsigned engine_scope_head;

/* Effective value of a destination after CV, as the engine last used it. */
extern volatile int16_t engine_value[CHANNELS][D_COUNT];

/* Voltage seen by a CV source for a channel (0 for Off). */
float engine_cv(int src, int ch);

float fast_exp2(float x);

/* Channel ch's sample voice for diag reports: plane, position and play
 * limit in frames, flags (on, tail << 4, rev << 8, looping << 12, sample
 * index << 16). */
void engine_diag(int ch, uint32_t w[4]);

#endif
