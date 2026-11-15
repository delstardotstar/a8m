#ifndef R80_H_INCLUDED
#define R80_H_INCLUDED

#include <stdint.h>

#include "r80_gen.h"

/* The "80" reverb unit (ref/reverb-80, algorithms-and-programs.md):
 * its reverb algorithms (Chamber, Plate, Concert Hall, Inverse tanks,
 * the input/output sections of 3 and 3.1), their factory programs as
 * stored parameters, modulators and patches, the parameter laws of 4,
 * the control layer of 6 at 48 kHz. */

enum r80_algo { R80_CHAMBER, R80_PLATE, R80_HALL, R80_INVERSE, R80_CHORUS, R80_ALGOS };

struct r80_patch {
    uint8_t source, dest, points, beats;    /* beats: points are 32 num + den codes (16.6) */
    uint8_t x[R80_POINTS_MAX];
    uint16_t y[R80_POINTS_MAX];
};

struct r80_program {
    uint16_t number;
    char name[13];
    uint8_t algo;
    uint16_t tempo;                         /* BPM */
    char adjust_label[12];
    uint8_t adjust, adjust_max;             /* the ADJUST knob's stored position */
    uint8_t trig_src[3], trig_level[3];     /* AR, Sw 1, Sw 2 (6.3) */
    uint8_t latch_src, latch_low, latch_high;
    uint8_t tap[4];                         /* Concert Hall: each modulated tap's window position */
    uint16_t value[R80_PARAMS];             /* stored; a locked one is 32 den + num */
    uint32_t locked[4];
    uint8_t npatches;
    struct r80_patch patch[R80_PATCHES_MAX];
};

extern const struct r80_program r80_programs[];
extern const unsigned r80_program_count;
const struct r80_program *r80_find(unsigned number);

/* ---- The Reverb page's fields: a program's main parameters ---- */

/* Edit slots: the parameters the page can edit (keys Reverb<key>), in
 * preset.reverb_edit[]. Append only. */
enum r80_edit {
    R80_E_ADJUST, R80_E_MIDRT, R80_E_SIZE, R80_E_PREDELAY, R80_E_HIGHCUT, R80_E_RTHC, R80_E_LOWRT, R80_E_DIFFUSION,
    R80_E_ATTACK, R80_E_SHAPE, R80_E_DURATION, R80_E_LOWSLOPE, R80_E_MIDSLOPE, R80_E_DEPTH, R80_E_MSTDEPTH,
    R80_E_MSTRATE, R80_E_DLYMASTER, R80_E_FBMASTER, R80_E_FXMIX, R80_EDITS
};
#define R80_FIELDS_MAX  9

/* The page's fields for program p (ADJUST first); returns how many. */
unsigned r80_fields(const struct r80_program *p, uint8_t edit[R80_FIELDS_MAX]);
/* The field's title ("MID RT"; ADJUST: the program's knob label). */
const char *r80_edit_name(const struct r80_program *p, unsigned e);
/* The yml key after "Reverb" ("MidRt"). */
const char *r80_edit_key(unsigned e);
void r80_edit_range(const struct r80_program *p, unsigned e, int *lo, int *hi);
/* The program's value (patches at rest, ADJUST at its stored position). */
int r80_edit_base(const struct r80_program *p, unsigned e);
/* The value as shown (out holds 16). */
void r80_edit_format(const struct r80_program *p, unsigned e, int v, char *out);

/* ---- The engine ---- */

/* Foreground. Starts a program (0: none) from silence: the memories
 * clear over the next blocks, silent until then; edits cleared. */
void r80_load(const struct r80_program *p);
/* An edit of field e, -1 for the program's value; it reaches the signal
 * by the parameter's own path (6.4). */
void r80_set(unsigned e, int value);
/* Foreground, often (the main loop): works out the laws for the values
 * the modulators, patches and edits give now and hands them to the audio. */
void r80_poll(void);
/* Field e's value playing now (modulated by the program's patches), -1
 * when no program runs. */
int r80_live(unsigned e);
/* The program r80_live speaks for (the last r80_load). */
const struct r80_program *r80_running(void);
/* One block: send[f][0] Q31 (the mono send), wet[f][side] Q30 (Q15 x
 * 2^15, as the DEP-5's returns). */
void r80_block(const int32_t (*send)[2], int32_t (*wet)[2], unsigned count);
/* Main loop, while the renderer is held off (engine_hush): the running
 * program starts over from silence. */
void r80_flush(void);

/* ---- Laws (r80law.c) ---- */

/* Input/output-section values, in the program data's key order. */
enum {
    R80_IO_ALL, R80_IO_ALR, R80_IO_ARL, R80_IO_ARR, R80_IO_GS, R80_IO_GR, R80_IO_POSTL, R80_IO_POSTR, R80_IO_PD,
    R80_IO_PR, R80_IO_C1, R80_IO_C2, R80_IO_G, R80_IO_VD, R80_IO_VP, R80_IO_DV1, R80_IO_V1L = R80_IO_DV1 + 4,
    R80_IO_FV = R80_IO_V1L + 12, R80_IO_FR, R80_IO_A, R80_IO_W1, R80_IO_W2, R80_IO_OUT, R80_IO3_COUNT
};
enum {
    R80_C_90, R80_C_91, R80_C_92, R80_C_93, R80_C_94, R80_C_FB1, R80_C_DAMP = R80_C_FB1 + 6, R80_C_B1,
    R80_C_GLIDE = R80_C_B1 + 6, R80_C_A1, R80_C_R1 = R80_C_A1 + 6, R80_C_38 = R80_C_R1 + 6, R80_C_U, R80_C_U0,
    R80_C_N, R80_C_CL1, R80_C_CR1 = R80_C_CL1 + 6, R80_C_AC = R80_C_CR1 + 6, R80_C_AD, R80_C_AE, R80_C_AF, R80_C_OUT,
    R80_IO31_COUNT
};
#define R80_K       127
#define R80_IO_MAX  64

/* Parameter values: 1/32 units (the voices' and post-delays' patch
 * resolution, 4.10); a locked one is the patch word 256 num + den. */
struct r80_values {
    int32_t v[R80_PARAMS];
    uint32_t locked[4];
};

/* The modulators' settings (6.3) as the control step uses them. */
struct r80_mod {
    uint32_t lfo_inc, sw_inc[2];
    uint16_t lfo_pw, lfo_dm, env_r[2], ar_a, ar_r, sw_pw[2], sw_up[2], sw_down[2];
    uint8_t lfo_shape, ar_mode, sw_mode[2];
};

/* What the laws give for a set of values. */
struct r80_target {
    int16_t k[R80_K];           /* tank coefficients x 256 (AP Sweep pairs at rest) */
    uint16_t a[R80_K];          /* tank offsets */
    double io[R80_IO_MAX];      /* 3 or 3.1 */
    struct r80_mod mod;
    uint16_t post_range, voice_range;   /* glide ranges, samples (4.13) */
    uint16_t post_resp, voice_resp;     /* G[GldResp] */
    uint8_t apsweep, divider, clear;    /* AP Sweep, the Hall taps' D (0 stopped), Clear */
    uint16_t geometry;                  /* counts Size / Duration changes: the loop restarts */
};

static inline int r80_locked(const struct r80_values *v, unsigned p)
{
    return (v->locked[p >> 5] >> (p & 31)) & 1u;
}

/* The values as stored. */
void r80_stored(const struct r80_program *p, struct r80_values *v);
/* A patch's value in its destination's units (1/32, or a beat word) for
 * source value x. */
int32_t r80_patch_value(const struct r80_patch *q, unsigned x, int locked);
/* Applies the patches with sources src[] (-1: no value) to v (from the
 * stored values s). */
void r80_patch_all(const struct r80_program *p, const struct r80_values *s, const int16_t *src,
                   struct r80_values *v);
/* The tank offsets (offsets 1) and the rest (offsets 0) for the values. */
void r80_resolve(const struct r80_program *p, const struct r80_values *v, struct r80_target *t, int offsets);
/* RT[Mid Rt] in ms. */
extern const uint16_t r80_rt_ms[64];
/* The parameter an edit slot sets; a value in whole units (a locked time
 * as its 2-ms steps); an AP Sweep pair's first coefficient from the
 * second's gain where it is coded as a half (6.1), x 256. */
unsigned r80_edit_param(unsigned e);
int r80_plain(const struct r80_program *p, const struct r80_values *v, unsigned q);
int r80_ap_first(int g);

#ifdef R80_HOST
/* The test's view: tap j's two read offsets and fraction code; whether
 * the memory has cleared since the load; the engine's targets. */
void r80_taps(unsigned j, unsigned *a, unsigned *b, unsigned *code);
int r80_ready(void);
const struct r80_target *r80_now(void);
void r80_hold(int taps, int sweep);
#endif

#endif
