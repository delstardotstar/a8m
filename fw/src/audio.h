#ifndef AUDIO_H_INCLUDED
#define AUDIO_H_INCLUDED

#include <stdint.h>

#define AUDIO_RATE      48000
#ifndef AUDIO_BLOCK
#define AUDIO_BLOCK     32      /* frames per render call; 16 halves gate latency but costs ~30 % CPU on hardware (cache), 2026-11-14 */
#endif
/* Blocks per 32 frames (the old block size): per-block counts and rates
 * written for 32 scale by it to keep their time. */
#define BLOCKS_PER_32   (32 / AUDIO_BLOCK)
/* A decay of k per 32 frames as the factor per block (a constant). */
#define BLOCK_DECAY(k)  __builtin_powf((k), AUDIO_BLOCK / 32.0f)
#define AUDIO_OUTPUTS   8       /* channel outputs 1-8 */

/* One block: out[frame][output] for the 8 channel outputs, mix[frame][0/1]
 * for mix left/right, in[frame][0/1] the sample input (one block late).
 * Full-scale is INT32_MIN..INT32_MAX. Runs in the EDMA interrupt. */
typedef void (*audio_render_fn)(int32_t out[][AUDIO_OUTPUTS], int32_t mix[][2],
                                const int32_t in[][2], unsigned frames);

/* Brings up the converters and both McASPs; render is then called once per
 * AUDIO_BLOCK frames. */
void audio_start(audio_render_fn render);
uint32_t audio_blocks(void);
#define AUDIO_TICKS_PER_FRAME (24000000u / AUDIO_RATE)    /* time_ticks() */
/* In render: when the block before the one being rendered started playing
 * (time_ticks). Events from the AUDIO_BLOCK frames up to then land at the
 * same offset in this block: a constant 2-block latency. */
uint32_t audio_block_ticks(void);
/* Drives the output un-mute line (GPIO2_24) once audio has been running for
 * a while, as stock does; returns 1 once un-muted. Call from the main loop. */
int audio_unmute_poll(void);
/* Drives the un-mute line low for good (before a reset). */
void audio_mute(void);
/* Timing health, for the glitch hunt. late_max: worst frames already
 * played from the new half when the ISR ran (AUDIO_BLOCK = a block lost). */
struct audio_stats {
    uint32_t late_max, repeats, underruns, missed, isr_us_max;
    uint32_t in_late;               /* sample input read over 2 ms late */
    uint32_t in_lost;               /* ... so late that frames were lost */
    uint32_t m1_blocks, m1_xstat, m1_rstat, m1_gblctl;     /* mix port */
    /* McASP1 watchdog: restarts, and the state at the last stall
     * (edma: ER, EER, SER, EMR bits for events 10/11 in nibbles 0-3). */
    uint32_t m1_restarts, m1_restart_fail;
    uint32_t m1_stall_xstat, m1_stall_rstat, m1_stall_gblctl, m1_stall_edma, m1_stall_ccnt;
};
extern struct audio_stats audio_stats;
/* Counts EDMA missed events and McASP0 underruns; call from the main loop. */
void audio_poll_errors(void);
/* Non-fatal bring-up problem to show, or 0. */
extern const char *audio_warning;
/* 1 if GPIO2_10 (board revision strap) read high at bring-up. */
extern int audio_board_rev_b;

#endif
