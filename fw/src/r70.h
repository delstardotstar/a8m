#ifndef R70_H_INCLUDED
#define R70_H_INCLUDED

#include <stdint.h>

#include "r70_gen.h"

/* The "70" reverb unit (ref/reverb-70, algorithms-and-programs.md):
 * its eight networks in the unit's integer arithmetic at its own rate
 * (33 854.17 Hz, resampled from and to the bus), the parameter laws of
 * sections 6-7, the level detector of 8, the delay modulation of 9, and
 * its 84 factory programs (sets A and B). */

struct r70_target { uint8_t kind, idx, law, arg; };     /* kind 0 a coefficient, 1 a delay */

struct r70_param {
    uint8_t pos;                    /* row * 10 + column */
    int16_t lo, hi;
    uint8_t role, flags;            /* R70_F_* */
    int16_t b_a;                    /* control value b: b_tab >= 0 ? r70_tables[b_tab + i] : b_a * i + b_c */
    int32_t b_c, b_tab;
    int16_t c_a;                    /* the count, the same way */
    int32_t c_c, c_tab;
    uint8_t nctl;
    struct { uint8_t field; int32_t tab; } ctl[2];      /* tab < 0: the control value is b */
    uint16_t tgt;
    uint8_t ntgt, ntaps, taps[6];   /* taps: predelay / reflections, limited by SIZE */
    uint8_t r_unit, r_offset, r_max_high;
    uint16_t r_max_beats;
    uint8_t r_taps[6];
    int32_t text;                   /* r70_texts index of its lo value, -1 none */
    const char *name;
};
enum { R70_F_B = 1, R70_F_COUNT = 2, R70_F_MASTER = 4 };

struct r70_base { uint8_t kind, idx; int16_t value; };

struct r70_algo {
    const char *name;
    uint8_t setb, net, ui;
    const struct r70_param *params;
    uint8_t nparams;
    const struct r70_base *base;
    uint8_t nbase;
    uint8_t rtsym[2];               /* the RT MID parameter's symbols the detector rewrites, 255 none */
    uint8_t has_rtlow, rtlow, has_chorus, chorus, has_dopt, dopt;   /* fixed control values */
    uint8_t nfields;
    uint8_t field_edit[R70_FIELDS_MAX], field_param[R70_FIELDS_MAX];
};

/* A ramped product's own coefficient (1/32): the delay it ramps for (255
 * none), the symbol it follows (255: the fixed value), whether the bypass
 * switch ramps it. */
struct r70_slot { uint8_t delay, sym; int8_t value; uint8_t res, byp; };
struct r70_mod { uint8_t k, A, B, a0, b0, S0; int16_t offset; };

struct r70_netinfo {
    uint8_t ncoef, ndelay, nslots;
    const struct r70_slot *slots;
    uint8_t nmods;
    struct r70_mod mods[6];
    uint8_t det, det_e, det_c;
    uint16_t tr, tw;
    uint8_t stages, shelf, ninloop, inloop[2], slope[2];
    uint8_t gdelay;
    uint16_t gfirst;
    uint8_t gpass, grest;
    const uint16_t *size, *sizemax;
    const uint8_t *sizef1, *sizetd, *sizeoff;
    uint8_t nsized, nsizeoff, smax, sb, scap;
    int16_t moff[2];
    uint8_t res[R70_COEFS];
};

struct r70_program {
    uint16_t number;
    char name[13];
    uint8_t algo;                   /* the page's algorithm (enum r70_algo) */
    uint8_t jalgo;                  /* r70_algos index */
    uint8_t setb, nvalues;
    int16_t value[R70_PARAMS];      /* stored, in r70_algos[jalgo].params order */
    uint8_t npatches;
    struct { uint8_t source, dest; int8_t scale; } patch[R70_PATCHES_MAX];
};

extern const struct r70_algo r70_algos[];
extern const struct r70_netinfo r70_netinfo[R70_NETS];
extern const struct r70_target r70_targets[];
extern const int32_t r70_tables[];
extern const char *const r70_texts[];
extern const char *const r70_algo_names[R70_ALGOS];
extern const char *const r70_edit_keys[R70_EDITS];
extern const struct r70_program r70_programs[];
extern const unsigned r70_program_count;
extern const uint16_t r70_tempo[128], r70_rt_rate[32];
extern const uint8_t r70_attack[2][4][256];
extern const uint8_t r70_sequence[2][2048];
extern const float r70_kdown[R70_PHASES + 1][R70_DOWN_TAPS], r70_kup[R70_PHASES + 1][R70_UP_TAPS];
extern const float r70_fxadj[103];
const struct r70_program *r70_find(unsigned number);

/* ---- The Reverb page's fields ---- */

/* An edit is the parameter's value index (v - lo, 0..hi - lo); -1 the
 * program's own value. A MASTER moves its row's voices with it. */
unsigned r70_fields(const struct r70_program *p, uint8_t edit[R70_FIELDS_MAX]);
const char *r70_edit_name(const struct r70_program *p, unsigned e, char *out);   /* out holds 16 */
const char *r70_edit_key(unsigned e);
void r70_edit_range(const struct r70_program *p, unsigned e, int *lo, int *hi);
int r70_edit_base(const struct r70_program *p, unsigned e);
void r70_edit_format(const struct r70_program *p, unsigned e, int v, char *out);   /* out holds 16 */

/* ---- Laws (r70law.c) ---- */

/* What a program and its edits give: every network value (coefficients in
 * units of their resolution, delays in samples), the control values, the
 * effective parameter values. */
struct r70_values {
    int16_t coef[R70_COEFS];
    uint16_t delay[R70_DELAYS];
    int16_t ctl[R70_CTLS];
    int16_t eff[R70_PARAMS];
    uint8_t jalgo, net;
    float fxgain;
};
void r70_build(const struct r70_program *p, const int16_t *edit, struct r70_values *out);
/* The RT-dependent values (7.6) for RT LOW / RT MID control values l, m,
 * DEFINITION d and DECAY OPT count n, into coef. */
void r70_rt(const struct r70_algo *a, int l, int m, int d, int n, int16_t *coef);
/* The effective value's display text. */
const char *r70_text(const struct r70_param *q, int v);

/* ---- The engine ---- */

/* Foreground. Starts a program (0: none) from silence; edits cleared. */
void r70_load(const struct r70_program *p);
void r70_set(unsigned e, int value);
/* Foreground, often: works out the laws for the edits and hands them to
 * the audio. */
void r70_poll(void);
int r70_live(unsigned e);
const struct r70_program *r70_running(void);
/* One block: send[f][0] Q31 (the mono send), wet[f][side] Q30. */
void r70_block(const int32_t (*send)[2], int32_t (*wet)[2], unsigned count);
/* Main loop, while the renderer is held off (engine_hush): the running
 * program starts over from silence. */
void r70_flush(void);

/* What the generated networks read. */
struct r70_run {
    uint32_t t;
    int32_t bypass;
    int32_t mag[R70_COEFS], sgn[R70_COEFS], neg[R70_COEFS];
    uint32_t delay[R70_DELAYS];
};
/* The networks' delay lines: mirrored rings of tap values 4w + 3. */
extern int32_t r70_pool[R70_POOL];

#ifdef R70_HOST
/* The test's view: run the network alone at the unit's rate (x: 16-bit
 * input samples), recording the network values in effect for every
 * sample block of 8; the detector's RT control values. */
void r70_native(const int16_t *x, int16_t (*y)[2], unsigned count, struct r70_run *snap, unsigned nsnap);
int r70_ready(void);
void r70_detector(int *curl, int *curm, int *n);
/* The network replaced by a wire (the resamplers alone). */
void r70_identity(int on);
#endif

#endif
