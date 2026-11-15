#ifndef OPS_H_INCLUDED
#define OPS_H_INCLUDED

#include "preset.h"

/*
 * Pure helpers of the Channel/Zone Ops and truncation. Freestanding
 * (host-tested by tools/ymltest.sh).
 */

#define SLICES_MAX  8

/* Boundaries b[0..n] of n slices of [start, end): b[0] = start, b[n] = end. */
void slices_equal(uint32_t start, uint32_t end, int n, uint32_t *b);

/*
 * Onset-based slicing of frames [start, end) of a sample (planes in the
 * samples.h Q31 format; r == l for mono) into n slices: the n-1 strongest
 * onsets, at least a minimum spacing apart; fewer onsets than that are
 * made up by halving the longest slice. b as for slices_equal.
 *
 * Detector: the signal (L+R)/2 is cut into hops of 256 frames (doubled
 * until there are at most 8192) and each hop's mean absolute first
 * difference (a high-pass energy, so tonal bass and DC don't hide a hit) is
 * taken in log2 against a floor 80 dB below the loudest hop. A hop's onset
 * strength is its rise over the mean of the three hops before it. Local
 * maxima above 0.25 (1.5 dB) are candidates; the strongest are taken
 * greedily when they are at least max(30 ms, 1/(8n) of the region) from
 * those taken and from both ends. Each is then moved to the first frame
 * within a hop either side whose first difference reaches a quarter of
 * that neighbourhood's largest.
 */
void slices_chop(const int32_t *l, const int32_t *r, uint32_t start, uint32_t end, uint32_t rate, int n,
                 uint32_t *b);

/*
 * A zone's points for a sample cut to frames [from, to) of its old_frames:
 * everything is moved down by from and clamped into the new length; an end
 * or loop end at the new end becomes POS_END.
 */
void zone_trim(struct zone_cfg *z, uint32_t from, uint32_t to, uint32_t old_frames);

/*
 * A sampler channel's zone list: zones 0..n-1 have a sample, the last one's
 * MinVoltage is -5 V. A Stereo/Right side applies the same operations to
 * its own list (stock 2.0), so each side keeps its own boundaries.
 */
int zones_count(const struct sampler_cfg *s);

/* Appends z as the new last zone; the zone above takes the lower half of
 * its range. Returns its index, -1 if all are used. */
int zones_append(struct sampler_cfg *s, const struct zone_cfg *z);

/* Removes zone z; the ones below move up with their own settings. */
void zones_delete(struct sampler_cfg *s, int z);

/* Duplicates zone z in front of itself: the copy takes the upper half of
 * the zone's range; the last zone is lost when all are used. */
void zones_insert(struct sampler_cfg *s, int z);

/* Reverses the order of zones z..z+n-1, keeping their voltage ranges. */
void zones_flip(struct sampler_cfg *s, int z, int n);

/* The Stereo/Right zone that follows master zone m: a copy, playing the
 * other plane of a stereo sample, else the same file. */
void zone_follow(struct zone_cfg *r, const struct zone_cfg *m, int stereo);

/*
 * After master m's zone z got a sample (old: the samples[] index + 1 it
 * had, 0 for a zone added when m had n zones), Stereo/Right side r follows
 * as stock 2.0 does: its zone z becomes zone_follow of it (positions and
 * MinVoltage included). A mono sample is followed only where r played the
 * master's old sample, or for an added zone when r had n zones too; a
 * stereo one always. A zone one past r's last is appended (halving r's own
 * range above). Returns 1 if r changed.
 */
int zones_sr_follow(const struct sampler_cfg *m, struct sampler_cfg *r, int z, int old, int n, int stereo);

#endif
