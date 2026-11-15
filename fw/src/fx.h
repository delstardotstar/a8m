#ifndef FX_H_INCLUDED
#define FX_H_INCLUDED

#include <stdint.h>
#include "audio.h"

/* Tallent Edition effects (fw/tallent): per channel Filter, EQ and
 * Compressor on each channel's output, after its level, before pan/mix;
 * Reverb and Delay sends returning to the mix, then the Mix Compressor. */

struct grain_tuning;
/* Audio IRQ: a sample channel's grain tuning (fw/tallent/grains.h), 0
 * while its grains are off (or it is a machine or Stereo/Right channel). */
const struct grain_tuning *fx_grain(int channel);
/* Main loop: publishes changed settings (and CV-moved ones) to the
 * renderer. */
void fx_poll(void);
/* Audio IRQ: whether fx_render must run this block, and the channels whose
 * filter or EQ runs (or fades); the others pass through untouched. */
int fx_busy(uint8_t *lanes);
/* Audio IRQ, once a block: the mix stages that run (FXM_*), and the
 * channels the compressors write (comp_lanes). */
enum { FXM_CHANNELS = 1, FXM_MIX = 2, FXM_EFFECTS = 4 };
unsigned fx_mix_busy(unsigned frames, uint8_t *comp_lanes);
/* Runs the eight channel buffers (post-level, 1.0 = full scale) through
 * the filters and EQs (fx) and the compressors (FXM_CHANNELS in mix), in
 * place for the channels in lanes. */
void fx_render(float (*buf)[AUDIO_BLOCK + 4], unsigned frames, int fx, uint8_t lanes, unsigned mix,
               const int32_t (*in)[2]);
/* A channel's sends: 1 Pre (before its mix level), 0 Post. */
int fx_send_pre(int channel);
/* The mix mix[frame][side] (Q31): the returns of the sends send[frame][channel]
 * (Q31) when FXM_EFFECTS, then the mix compressor. */
void fx_mix_render(int32_t (*mix)[2], const int32_t (*send)[8], unsigned frames, unsigned flags);
/* A compressor's gain reduction now, whole dB: a channel, or CHANNELS for
 * the mix. */
unsigned fx_reduction_db(int compressor);
/* Main loop, while the renderer is held off (engine_hush): every effect's
 * memory back to silence (filters, EQs, compressors, reverb, delay). */
void fx_flush(void);

#ifdef FX_TEST
void fx_selftest(void);
#endif
#ifdef MIXFX_TEST
void mixfx_selftest(void);
#endif

#endif
