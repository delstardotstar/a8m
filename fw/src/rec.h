#ifndef REC_H_INCLUDED
#define REC_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include "audio.h"

/*
 * Sampling (stock's Sampling Setup). The audio interrupt decimates the
 * 192 kHz sample input (McASP1) to 96 and 48 kHz (half-band FIRs), gives the
 * engine its 48 kHz block, and while capturing applies level and high-pass
 * and writes the chosen rate into a capture ring. The main loop moves the
 * ring into sample memory (record modes, pre-roll), turns the take into a
 * sample and has storage write it as a 24-bit WAV.
 */

enum { RMODE_ONCE, RMODE_CIRCULAR, RMODE_SOS, RMODE_SOS_FADE, RMODE_COUNT };
enum { RRATE_48, RRATE_96, RRATE_192 };
enum { RNAME_NEW, RNAME_S, RNAME_PRESET };
enum { RSIDE_LEFT, RSIDE_RIGHT, RSIDE_STEREO, RSIDE_DUAL };
#define RDEST_NONE      0       /* 1-8: that channel */
#define RDEST_EMPTY     9
#define RTHRESH_MIN     (-60)   /* dB; 0 = off; 1..24 = CV input 1A..8C */
#define RTIME_MAX       6000    /* 0.1 s; 0 = All */
#define RLEVEL_MIN      (-30)
#define RLEVEL_MAX      24
#define RMON_OFF        (-60)   /* monitor level dB; this value is off */
#define RMON_MAX        12
#define RMON_SEND_MAX   100     /* % */

struct rec_setup {
    uint8_t dest, zone_empty, mode, rate, naming, hpf, hot, side, mon_l, mon_r;
    int16_t time, thresh;
    int8_t level;
    int8_t mon_level;           /* the monitor in the mix, dB */
    uint8_t mon_rev, mon_dly;   /* monitor sends, %, after mon_level */
};
extern struct rec_setup rec_setup;

enum { REC_IDLE, REC_ARMED, REC_RECORDING, REC_FINISHING, REC_SAVING };

/* Audio interrupt: n frames of the 192 kHz input; fills in48 with the
 * newest AUDIO_BLOCK frames at 48 kHz (for phase mod). */
void rec_input(const volatile int32_t (*x)[2], unsigned n, int32_t in48[][2]);
/* Audio interrupt, after rendering: adds the monitored input to the mix. */
void rec_monitor(int32_t mix[][2], unsigned frames);
/* Audio interrupt, from the mix effects: adds the monitored input (one
 * mono sum, after MON LEVEL) to the reverb and delay sends, Q31. */
void rec_monitor_send(int32_t send[][2], unsigned frames);
/* Main loop: whether the monitor feeds the sends. */
/* SAMPLING SETUP has the input monitor on (it may be held off). */
int rec_monitor_set(void);
int rec_monitor_sends(void);

/* Main loop. */
void rec_poll(void);
int rec_state(void);
/* Arm / disarm by hand (long press SAMPLING SETUP). *HOT* is always ready
 * for SAMPLE and CV; a threshold start needs arming by hand in both. */
void rec_arm(int on);
int rec_armed(void);            /* armed by hand, waiting */
/* SAMPLE button: starts when armed or *HOT*, stops (or sets the SOS loop)
 * when recording; ignored while a take is being saved. 0 if not for us. */
int rec_button(void);
/* The setup page shows the meters: keep the input processed. */
void rec_metering(int on);
/* Post-gain input peaks (0..1, decaying) and the threshold, linear. */
extern volatile float rec_peak[2];
float rec_thresh_lin(void);
/* While recording: frames at the take's rate so far, the capacity. */
uint32_t rec_frames(void);
uint32_t rec_capacity(void);
uint32_t rec_rate_hz(void);
/* The finished take's sample indices (right: Dual Mono's second, else -1),
 * once per take; 0 if none is waiting to be placed. */
int rec_take(int *left, int *right);
/* Saving progress, % (REC_SAVING). */
int rec_save_percent(void);
/* Why the last take failed or stopped short, or 0 (cleared on read). */
const char *rec_message(void);
/* Settings changed (written to 1:A8M-SSETUP.YML later). */
void rec_setup_changed(void);
/* Reads/writes the settings file (storage). */
void rec_setup_load(void);
/* A8M-SSETUP.YML text (src/ssetup.c): applies the lines of buf to r
 * (unknown keys and bad values ignored); writes r's text, 0 if cap is short. */
void ssetup_parse(const char *buf, size_t len, struct rec_setup *r);
size_t ssetup_text(const struct rec_setup *r, char *out, size_t cap);
#define REC_PROFILE_192 96      /* PROFILE_VOICE: input capture at 192 kHz (emu/voicecost.py) */
#define REC_PROFILE_48  95      /* ... at 48 kHz */
/* While rec_level_on: the last interrupt's mean input word per channel
 * (offset - raw, as stock's reading). */
extern volatile int rec_level_on;
extern volatile int32_t rec_level[2];
extern volatile uint32_t rec_level_epoch;
/* Capture cost in the interrupt, us per block (worst since reset). */
extern volatile uint32_t rec_us_max, rec_us_last;

#endif
