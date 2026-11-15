#ifndef YML_H_INCLUDED
#define YML_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "preset.h"

/*
 * prstNNN.yml <-> struct preset. Freestanding, no allocation, no FatFs.
 *
 * Stock keys used (stock formats, units; keys match case-insensitively;
 * stock omits defaults and so do we):
 *   preset   Name            text, 12 chars
 *            Data2asCV       "1A".."8C" (absent/"Off" = none)
 *   channel  Pitch           semitones, +0.00; cents = 100 x
 *            Level, MixLevel dB, +0.0; ours in tenths (MixLevel = mix out)
 *            Pan             -1.00..+1.00; ours -100..100
 *            Bits            0.1 steps; "32.0" = full (default)
 *            ChannelMode     0 Master, 1 Link, 2 Stereo/Right, 3 Cycle
 *            AutoTrigger     1 = Trigger On Select Auto (0 = Normal)
 *            PitchCV, BitsMod, PanMod, MixMod
 *                            "<src> <amount>", amount -2.00..2.00 in
 *                            stock's scales (engine.c per_volt; pitch
 *                            0.50 = 1 V/oct). src: 1A..8C absolute,
 *                            0A/0B/0C the channel's own inputs (stock
 *                            writes "0A"; "A", "CVA" also read). Written only when src != Off.
 * Our keys (channel; M-prefixed or "Machine" + control name, never a stock key):
 *   Machine         voice name ("TRX-BD"); "Off"/absent = sampler channel
 *   Machine<NAME>   control value 0..127, NAME a param of the voice's descriptor (voices.h)
 *   Machine<NAME>Mod  CV route, same syntax as PitchCV
 *   MLevelMod       CV route for Level (ours is dB/V; stock's LinAM/ExpAM differ)
 *   MMixOn          0 = channel muted from the mix (absent = 1)
 *
 * Sampler keys (struct sampler_cfg, stock formats and defaults, from
 * stock's own save in the emulator, tmp/samp2/probe-saved.yml):
 *   channel  Attack, Release "%7.4f" s (default 0, 0.1); AttackFromCurrent,
 *            Aliasing, PlayMode (0 gated = default, 1 one shot), PMSource,
 *            LoopMode, LoopLengthIsEnd, Reverse, LinAMisExtEnv,
 *            SpliceSmoothing, ZonesRT, MixModIsFader "%d"; PMIndex "%04.2f";
 *            XfadeGroup A..D; ZonesCV "<src>" (no amount); MReverseCV "<src>" (ours); LinFM ExpFM
 *            PhaseCV PMIndexMod AliasingMod LinAM ExpAM AttackMod ReleaseMod
 *            SampleStartMod SampleEndMod LoopStartMod LoopLengthMod routes.
 *            Channel-level SampleStart/SampleEnd/LoopStart/LoopLength are
 *            zone 1's (stock moves them there on save).
 *   zone     SampleStart, SampleEnd, LoopStart "%d" frames (End default =
 *            the sample's end); LoopLength "%11.4f" frames in 1/32 steps
 *            (only values that print back identically, else verbatim);
 *            Side "%d"; PitchOffset "%+04.2f"; LevelOffset "%+04.1f";
 *            MinVoltage "%+.2f".
 * Tallent Edition lines (its PRESET_FORMAT.md, read in any section, written
 * after the channels behind "ExtrasFormat : 3"): EQ<c>, EQCV<c>, FILT<c>,
 * FILTCV<c>, FILTR<c> and their Z<z> zone forms into eq/filter, then
 * Grain<c>, GrainCV<c> (format 1's Grain, GrainCV too) into grain; kept
 * TimeLock<c>, TimeLock<c>Z<z> into tl and zone_cfg.beats; kept lines
 * follow, then MixFX<n> (mixfx).
 * Everything else (zone Sample lines, unknown keys and any known key whose
 * value is malformed or outside our range) is kept verbatim in a struct
 * yml_keep and written back in its section.
 * Machine lines naming no voice are kept too. Our keys follow the kept lines
 * of each section so they win on a duplicate.
 */

#define YML_OK          0
#define YML_E_ARG       (-1)
#define YML_E_FORMAT    (-2)    /* no preset/channel/key line at all */
#define YML_E_SPACE     (-3)    /* output buffer too small */

/* Kept lines: a bounded pool of (section, length, text) records, text
 * trimmed of indent and line end, at most 255 bytes each. A full stock
 * preset (8 channels x 8 zones, every key present) needs about 20 KB;
 * typical stock presets (defaults omitted) under 2 KB. Lines that don't
 * fit are dropped and counted in `lost`, as are lines in a Channel/Zone
 * number out of range. Size one with YML_KEEP_SIZE(cap) bytes. */
struct yml_keep {
    uint32_t cap;
    uint32_t used;
    uint32_t lost;
    uint8_t chan_seen;          /* bit c: "Channel c+1" header present */
    uint8_t zone_seen[CHANNELS];    /* bit z: "Zone z+1" header present */
    uint8_t pool[];
};

#define YML_KEEP_SIZE(cap)  (sizeof(struct yml_keep) + (cap))
#define YML_KEEP_FULL       20480u

void yml_keep_init(struct yml_keep *k, uint32_t cap);

/* Parses one preset file. p is fully initialised (absent keys: sampler
 * channel, stock defaults); all fields are in range afterwards. Malformed
 * input never fails beyond YML_E_FORMAT. k may be NULL (kept lines dropped). */
int yml_parse(const char *buf, size_t len, struct preset *p, struct yml_keep *k);

/* Sample names of the zones (Zone sections' `Sample : name.wav`, kept
 * lines), "" where none. Names longer than YML_SAMPLE_LEN (stock's limit)
 * are left out. Returns how many zones name a sample. */
#define YML_ZONES       8
#define YML_SAMPLE_LEN  47
struct yml_samples {
    char name[CHANNELS][YML_ZONES][YML_SAMPLE_LEN + 1];
};
int yml_samples(const struct yml_keep *k, struct yml_samples *s);

/* When set, yml_write names zone samples from zone_cfg.smp through it
 * (kept Sample lines are dropped) and leaves out zones without one. */
extern const char *(*yml_sample_name)(unsigned smp);

/* Writes the file (LF, stock indentation) as "Preset <number> :". k may be
 * NULL. Returns the length (no terminator) or YML_E_SPACE. */
int yml_write(const struct preset *p, const struct yml_keep *k, int number,
              char *out, size_t cap);

#endif
