#ifndef TLOCK_H_INCLUDED
#define TLOCK_H_INCLUDED

#include <stdint.h>

#include "timelock.h"

/*
 * Time Lock (Tallent, fw/tallent/timelock.c) on sample channels: the
 * foreground finds each locked sample's hits, builds the zones' tempo
 * tables and plans the joins; the audio interrupt runs the clocks and,
 * through engine.c, the locks a frame at a time.
 */

/* A channel's lock for this block (tlock_begin). */
struct tlock_ch {
    struct lock *l;
    const struct lock_zone *z;
    const struct clock_now *k;      /* the clock at each frame of the block, or 0 */
    uint32_t zone, pair, own, streams;
};

/* Main loop. */
void tlock_poll(void);
/* Audio IRQ: the clocks over the block's frames, before any channel; in: the Sample input. */
void tlock_block(unsigned frames, const int32_t (*in)[2]);
/* Audio IRQ: channel ch's lock for this block when it applies to the voice
 * (playing zone, its start/end/loop in Q32 frames, forward), else 0 (the
 * lock let go). step: the block's first step. */
int tlock_begin(int ch, int zone, int64_t start, int64_t end, int64_t lstart, int64_t lend, int looping,
                int32_t step, struct tlock_ch *out);
void tlock_idle(int ch);
/* Its table has ch on with a Stereo/Right partner (render order). */
int tlock_pair(int ch);

/* A channel's tempo for a zone, as the Time Lock page shows it (Tallent's
 * struct timelock_tempo): beats (guessed: Auto), the loop's own BPM and the
 * one it plays at, in tenths; clocked: follows a clock; clock_live. */
struct tlock_tempo {
    uint32_t beats, own_bpm10, bpm10;
    uint8_t guessed, clock_live, clocked;
};
void tlock_tempo(int ch, int zone, struct tlock_tempo *out);
/* Hits found for the channel's zone: 1, 0 still looking, -1 no sample. */
int tlock_mapped(int ch, int zone);

#endif
