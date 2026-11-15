/* Local: upstream's ext/mix_fx.h without the channel copies, remote edits,
 * MIDI clock and page; the settings are preset.mixfx (fw/src/preset.h); the
 * hooks at the end are ours (fx.c).
 *
 * Mix effects: a compressor on each channel and on the mix, and two sends
 * on the mix, a reverb and a tempo-synced delay. mix_fx.c runs them in the
 * audio interrupt; mix_page.c edits the settings. */
#ifndef MIX_FX_H
#define MIX_FX_H
#include "eq.h"

#define MIX_CHANNELS        8u
#define MIX_RATIOS          9u   /* Off, 1.5:1, 2:1, 3:1, 4:1, 6:1, 10:1, 20:1, Limit */
#define MIX_ATTACKS         11u
#define MIX_RELEASES        7u
#define MIX_THRESHOLD_MAX   40u  /* dB below full scale */
#define MIX_KEY_SELF        0u   /* 1-8: that channel */
#define MIX_KEY_INPUT       9u   /* the Sample input, left and right */
#define MIX_DIVISIONS       11u
#define MIX_FILTER_STEPS    7u
#define MIX_PERCENT_MAX     100u
#define MIX_FEEDBACK_MAX    120u
#define MIX_DECAY_MIN       2u   /* tenths of a second */
#define MIX_DECAY_MAX       100u
#define MIX_BPM_MIN         40u
#define MIX_BPM_MAX         240u
/* The delay's Tempo: a BPM, MIDI clock, or (ours) a CV CLOCK on
 * delay_clk_src: a CV input 0-23 (1A, 1B, 1C, 2A .. 8C) or the Sample
 * input's left (24) or right (25) at delay_clk_ppqn pulses a beat (0:
 * TIME_LOCK_PPQN_DEFAULT). Values 1-8 were a channel's Time Lock tempo
 * (Tallent) and now read as MIX_BPM_DEFAULT. */
#define MIX_TEMPO_MIDI      0u
#define MIX_TEMPO_BPM       9u   /* and above: MIX_BPM_MIN + (tempo - MIX_TEMPO_BPM) */
#define MIX_TEMPO_MAX       (MIX_TEMPO_BPM + MIX_BPM_MAX - MIX_BPM_MIN)
#define MIX_TEMPO_CV        (MIX_TEMPO_MAX + 1u)
#define MIX_BPM_DEFAULT     120u
#define MIX_CLK_INPUT_L     24u
#define MIX_CLK_INPUTS      26u

/* The BPM a delay_tempo value sets; 0 when it follows a clock. */
static inline u32 mix_tempo_bpm(u32 tempo)
{
    if (tempo == MIX_TEMPO_MIDI || tempo == MIX_TEMPO_CV) return 0;
    return tempo >= MIX_TEMPO_BPM && tempo <= MIX_TEMPO_MAX ? MIX_BPM_MIN + tempo - MIX_TEMPO_BPM : MIX_BPM_DEFAULT;
}

/* A CV route: an input selector (EQ_CV_SOURCE_*) and an amount, -100 to
 * 100. At +100, 5 V moves the setting across its whole range. */
struct mix_route { signed char source, amount; };
enum { MIX_ROUTE_REVERB_SEND, MIX_ROUTE_DELAY_SEND, MIX_SEND_ROUTES };
enum { MIX_ROUTE_REVERB_DECAY, MIX_ROUTE_REVERB_DAMP, MIX_ROUTE_REVERB_LEVEL, MIX_ROUTE_DELAY_TIME,
       MIX_ROUTE_DELAY_FEEDBACK, MIX_ROUTE_DELAY_LOW, MIX_ROUTE_DELAY_HIGH, MIX_ROUTE_DELAY_LEVEL,
       MIX_ROUTE_CHORUS_RATE, MIX_ROUTE_CHORUS_DEPTH, MIX_BUS_ROUTES };
/* The reverb's algorithms, in the DEP-5's order and numbering (10, chorus
 * and delay, is left out: the delay send covers it). */
#define MIX_ALGOS           10u
#define MIX_ALGO_GATED      6u   /* this index and above: Decay is the gate length */

/* One compressor's settings. */
/* One compressor's settings: Drive (a soft-clipping gain ahead of it),
 * Gain (makeup, dB) and Mix (wet percent, for parallel compression). */
struct mix_comp_set { u8 ratio, threshold, attack, release, drive, gain, mix, key; };
#define MIX_GAIN_MAX        24u

/* The settings live in the preset's extras (preset.mixfx), saved
 * as MixFX lines. A preset without them has zeros there: the defaults
 * apply until the first edit writes them. MIX_FORMAT changes when a field
 * moves; fields appended later read zero from older presets. */
#define MIX_FORMAT          1u
struct mix_settings {
    u8 format;
    struct mix_comp_set comp[MIX_CHANNELS], master;
    u8 reverb_send[MIX_CHANNELS], delay_send[MIX_CHANNELS];
    u8 reverb_algo, reverb_decay, reverb_damp, reverb_level, chorus_rate, chorus_depth;
    u8 delay_division, delay_feedback, delay_low, delay_high, delay_level, delay_tempo;
    struct mix_route send_route[MIX_CHANNELS][MIX_SEND_ROUTES], bus_route[MIX_BUS_ROUTES];
    u8 send_pre[MIX_CHANNELS];   /* 1: sends before the channel's level and mute; 0: after */
    u8 delay_clk_src, delay_clk_ppqn;
};
_Static_assert(sizeof(struct mix_settings) <= TA_MIXFX_BYTES, "mix settings outgrow the preset's extension bytes");
extern struct mix_settings mix_defaults;
/* A preset's settings (its MixFX bytes), or the defaults. */
static inline const struct mix_settings *mix_of(const u8 *mixfx)
{
    const struct mix_settings *m = (const struct mix_settings *)mixfx;
    return m->format == MIX_FORMAT ? m : &mix_defaults;
}
/* The settings in effect: the preset's, or the defaults. */
static inline const struct mix_settings *mix_now(void)
{
    return mix_of(preset.mixfx);
}
/* A channel's part, which moves with channel copies. */
struct mix_channel { struct mix_comp_set comp; u8 reverb_send, delay_send, send_pre; struct mix_route send_route[MIX_SEND_ROUTES]; };
void mix_channel_get(const struct mix_settings *from, u32 channel, struct mix_channel *out);
void mix_channel_put(u32 channel, const struct mix_channel *in);
/* The preset's settings to change, starting from the defaults. */
struct mix_settings *mix_edit(void);


extern const char *const mix_ratio_names[MIX_RATIOS];
extern const u16 mix_attack_hundredths_ms[MIX_ATTACKS];
extern const u16 mix_release_ms[MIX_RELEASES];
extern const char *const mix_division_names[MIX_DIVISIONS];
extern const char *const mix_algo_names[MIX_ALGOS];
/* Whether an algorithm has a chorus. */
u32 mix_algo_chorus(u32 algo);
/* A compressor's gain reduction now, whole dB: a channel, or MIX_CHANNELS for the mix. */
u32 mix_fx_reduction_db(u32 compressor);
/* The delay's cut frequencies in Hz; 0 is Off. */
extern const u16 mix_low_cut_hz[MIX_FILTER_STEPS], mix_high_cut_hz[MIX_FILTER_STEPS];

/* Marks the settings changed; the audio interrupt recalculates them. */
void mix_fx_update(void);
/* The tempo the delay follows, in tenths of BPM; 0 when its clock has stopped. */
u32 mix_fx_bpm10(void);
/* Follows the delay's tempo source, from the foreground. */
void mix_fx_foreground(void);
void mix_fx_init(void);
/* Silences the reverb, delay and compressors' memory; only while the
 * renderer is held off (engine_hush). */
void mix_fx_flush(void);

/* Ours (fx.c): whether channel is the right side of a stereo pair. The
 * renderer's stages, once a block: ta_mix_block first, then the channels
 * x[frame][channel] (Q31 at 2^30, after the filter and EQ) with the Sample
 * input in[frame][side], then the mix mix[frame][side] (Q31) with each
 * channel's send source send[frame][channel] (Q31, Pre or Post as set);
 * effects 0 runs only the mix compressor. ta_mix_bus returns the larger
 * return's peak this block, Q15. */
int ta_pair_right(u32 channel);
/* fw/src/tlock.c: a beat of MIDI clock, Q8 frames; 0 when stopped. */
u32 ta_midi_beat_q8(void);
/* The delay's CV CLOCK (MIX_TEMPO_CV): its beat in Q8 frames, 0 when the
 * clock has stopped; and the BPM to show in tenths of its clock (MIDI or
 * CV), 0 likewise. */
u32 ta_clock_beat_q8(void);
u32 ta_clock_bpm10(void);
void ta_mix_block(u32 frames);
/* The 80's reverb in place of this one for a block, when the preset picks
 * it: the reverb send send[frame][0] in, the returns into wet as
 * reverb_block leaves them. 0: not picked, reverb_block runs. */
u32 ta_reverb80(const s32 (*send)[2], s32 (*wet)[2], u32 frames);
void ta_mix_channels(s32 (*x)[MIX_CHANNELS], const s32 (*in)[2], u32 frames);
u32 ta_mix_bus(s32 (*mix)[2], const s32 (*send)[MIX_CHANNELS], u32 frames, u32 effects);
/* The Sample input monitor's part of the block's sends (reverb, delay). */
void ta_mix_monitor(s32 (*send)[2], u32 frames);

#endif
