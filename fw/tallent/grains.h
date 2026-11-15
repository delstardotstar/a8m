/* Local: the grain engine of upstream's ext/grains.c as a library (one
 * state per channel), its fields and ranges from src/patch.h and
 * src/preset_extras.c. fw/src/engine.c feeds it the voice's playhead and
 * steps; fx.c publishes the tunings. */
#ifndef TA_GRAINS_H
#define TA_GRAINS_H
#include "patch.h"
#include "preset.h"

enum { GRAIN_ON, GRAIN_SIZE, GRAIN_OVERLAP, GRAIN_SPRAY, GRAIN_DETUNE, GRAIN_OCTAVE, GRAIN_REVERSE,
       GRAIN_PAN, GRAIN_SHAPE, GRAIN_SCAN, GRAIN_SLIDE, GRAIN_SKEW, GRAIN_FIELDS };
enum { GRAIN_ROUTE_SIZE, GRAIN_ROUTE_OVERLAP, GRAIN_ROUTE_SPRAY, GRAIN_ROUTE_DETUNE, GRAIN_ROUTE_SLIDE,
       GRAIN_ROUTE_SCAN, GRAIN_ROUTE_SHAPE, GRAIN_ROUTE_PAN, GRAIN_ROUTES };
_Static_assert(GRAIN_FIELDS == TA_GRAIN_FIELDS && GRAIN_ROUTES == TA_GRAIN_ROUTES, "preset.h's grain_value");

struct grain_range { s16 low, high, initial; };
static const struct grain_range grain_ranges[GRAIN_FIELDS] = {
    {0, 1, 0},          /* this channel plays grains */
    {5, 20000, 600},    /* size, 0.1 ms */
    {25, 800, 400},     /* overlap, percent: grains sounding at once x100 */
    {0, 2000, 20},      /* spray, ms of random position */
    {0, 1200, 0},       /* detune, cents either way */
    {0, 100, 0},        /* grains an octave up, percent */
    {0, 100, 0},        /* grains played backwards, percent */
    {0, 100, 50},       /* pan spread, percent (stereo pair) */
    {1, 100, 100},      /* shape: percent of the grain spent fading */
    {-400, 400, 100},   /* scan, percent of the voice's speed */
    {-1000, 1000, 0},   /* slide, tenths of a percent of the zone added to the start */
    {-100, 100, 0},     /* skew: -100 all release (Decay), +100 all attack (Reverse) */
};
struct grain_route { u8 field; u16 span; };
extern const struct grain_route grain_routes[GRAIN_ROUTES];

#define GRAINS                 8u
#define GRAIN_STEP_SHIFT       25u         /* steps: Q25 frames per output frame */
#define GRAIN_PARKED           0xffffffffu /* a playhead with no voice behind it */
#define GRAIN_DETUNES          16u

struct grain_fades { u32 attack, attack_inverse, release, release_inverse, smooth; };
/* The per-frame values of a channel's fields (upstream: struct tuning). */
struct grain_tuning {
    u32 frames, phase_step, interval, norm, spray, octave, reverse;
    struct grain_fades fades;
    u32 scatter;
    s32 pan, scan, backwards;
    s32 slide;
    u32 detune[GRAIN_DETUNES];
};
/* The playing zone in the direction of play: frame i of it is pcm[i * dir]
 * (dir -1: a reversed voice), right words from left to the right side. */
struct grain_zone {
    const s32 *pcm;
    s32 right, dir;
    u32 frames;
    u32 loop, turn;
};
struct grain {
    const s32 *pcm;
    s32 right, dir;
    s64 position;
    s32 step;
    u32 phase, phase_step;
    struct grain_fades fades;
    u32 fade, fade_step;
    s32 gain[2][2];
};
struct grain_state {
    struct grain grains[GRAINS];
    s32 countdown, norm;
    s64 drift;
    u32 heard, seed, spawned, skipped, stolen;
};

u32 grain_ratio_q16(s32 cents);
/* The fields of g with its routes' CV added (CV A-C: channel's inputs). */
void grain_fields(const struct grain_value *g, u32 channel, s32 *values);
void grain_derive(const s32 *values, struct grain_tuning *t);
void grain_start(struct grain_state *s, const struct grain_tuning *t, u32 channel);
/* frames output frames: at[f] the playhead's frame in z (GRAIN_PARKED,
 * or past its end, when not playing), steps[f] the voice's signed step.
 * Q31 into left, and right when pair. */
void grain_render(struct grain_state *s, const struct grain_tuning *t, const struct grain_zone *z, int pair,
                  const u32 *at, const s32 *steps, s32 *left, s32 *right, u32 frames);
#endif
