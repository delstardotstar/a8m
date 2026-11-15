#ifdef FX_TEST
/* EXTRA=-DFX_TEST (emu/fxresp.py): the EQ and filter's measured response
 * on channel 1 against the graphs' curves (eq_curve, filter_curve), at
 * the curves' frequencies. Prints "R case point measured-cdB curve-cdB"
 * (the EQ curve stops at +-21 dB: "R ... E" marks its points). */
#include "engine.h"
#include "cv.h"
#include "eq.h"
#include "eq_design.h"
#include "fx.h"
#include "hw.h"
#include "preset.h"

#define POINTS      31
#define SETTLE      4800
#define WINDOW      9600
#define AMP         0.25f
#define EQ_ROWS     254
#define F_TOP_DB    30
#define F_BOT_DB    (-90)
#define F_ROWS      240

static float chbuf[CHANNELS][AUDIO_BLOCK + 4] __attribute__((aligned(16)));

static void put(const char *s)
{
    while (*s) {
        REG(0x44e09000u) = (uint32_t)*s++;
    }
}

static void put_int(int v)
{
    char d[12];
    int n = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;

    if (v < 0) {
        put("-");
    }
    do {
        d[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n) {
        char c[2] = { d[--n], 0 };
        put(c);
    }
}

/* Gain in hundredths of a dB of a sine at hz through channel 1. */
static int measure(double hz)
{
    double w = hz * (6.283185307179586 / 48000), c = cosine(w), s = sine(w);
    double re = 1, im = 0, ar = 0, ai = 0, ws = 0;
    int total = SETTLE + WINDOW;

    for (int ch = 0; ch < CHANNELS; ch++) {
        for (int f = 0; f < AUDIO_BLOCK + 4; f++) {
            chbuf[ch][f] = 0.0f;
        }
    }
    for (int n0 = 0; n0 < total; n0 += AUDIO_BLOCK) {
        double ph[AUDIO_BLOCK][2];

        for (int f = 0; f < AUDIO_BLOCK; f++) {
            double t = re * c - im * s;
            ph[f][0] = re;
            ph[f][1] = im;
            chbuf[0][f] = AMP * (float)im;
            im = im * c + re * s;
            re = t;
        }
        fx_render(chbuf, AUDIO_BLOCK, 1, 0xff, 0, 0);
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            int n = n0 + f - SETTLE;
            if (n >= 0) {
                double h = 0.5 - 0.5 * cosine(6.283185307179586 * n / WINDOW);
                ar += h * chbuf[0][f] * ph[f][0];
                ai += h * chbuf[0][f] * ph[f][1];
                ws += h;
            }
        }
    }
    double mag = 4.0 * (ar * ar + ai * ai) / (ws * ws) / (AMP * AMP);
    return (int)(logarithm(mag < 1e-14 ? 1e-14 : mag) * (1000.0 / 2.302585092994046));
}

static void settle(void)
{
    /* Publish, then let the renderer take the bank and finish its glide. */
    for (int k = 0; k < 3; k++) {
        fx_poll();
        for (int ch = 0; ch < CHANNELS; ch++) {
            for (int f = 0; f < AUDIO_BLOCK; f++) {
                chbuf[ch][f] = 0.0f;
            }
        }
        for (int b = 0; b < 40 * BLOCKS_PER_32; b++) {
            fx_render(chbuf, AUDIO_BLOCK, 1, 0xff, 0, 0);
        }
    }
}

static void run_case(int id, int is_eq)
{
    uint8_t rows[POINTS];
    struct channel_cfg *c = &preset.ch[0];

    settle();
    if (is_eq) {
        struct eq_value live;
        eq_modulate(&c->eq, 0, &live);
        eq_curve(&live, POINTS, rows, 0, EQ_ROWS);
    } else {
        struct filter_value live;
        filter_live(&c->filter, 0, &live);
        filter_curve(&live, POINTS, rows, 0, F_ROWS, F_TOP_DB, F_BOT_DB);
    }
    double hz = EQ_FREQUENCY_MIN, mul = exponential(6.907755278982137 / (POINTS - 1));
    for (int n = 0; n < POINTS; n++, hz *= mul) {
        int curve = is_eq ? (EQ_ROWS / 2 - rows[n]) * EQ_GRAPH_DB * 100 / (EQ_ROWS / 2)
                          : F_TOP_DB * 100 - rows[n] * (F_TOP_DB - F_BOT_DB) * 100 / F_ROWS;
        put("R ");
        put_int(id);
        put(" ");
        put_int(n);
        put(" ");
        put_int(measure(hz));
        put(" ");
        put_int(curve);
        put(is_eq ? " E\n" : "\n");
    }
}

void fx_selftest(void)
{
    struct channel_cfg *c = &preset.ch[0];
    int id = 0;
    static const int16_t gains[][3] = { { 60, -90, 80 }, { -100, 150, 0 }, { 0, 0, -100 } };
    static const uint8_t qs[][3] = { { 0, 0, 0 }, { 9, 25, 0 }, { 0, 0, 17 } };
    static const uint16_t hpfs[] = { 0, 120, 0 };

    for (int k = 0; k < 3; k++) {
        eq_value_defaults(&c->eq);
        c->eq.on = 1;
        c->eq.hpf = hpfs[k];
        for (int b = 0; b < 3; b++) {
            c->eq.gain[b] = gains[k][b];
            c->eq.q[b] = qs[k][b];
        }
        run_case(id++, 1);
    }
    /* Mid gain by CV: +2 V at +0.50 is +2.0 dB. */
    eq_value_defaults(&c->eq);
    c->eq.on = 1;
    c->eq.gain[1] = 60;
    c->eq.source[EQ_MOD_MID_GAIN] = 0;
    c->eq.amount[EQ_MOD_MID_GAIN] = 50;
    cv_volts[0] = 2.0f;
    run_case(id++, 1);
    cv_volts[0] = 0.0f;
    eq_value_defaults(&c->eq);
    settle();
    for (int t = FILTER_LP24; t < FILTER_TYPES; t++) {
        filter_value_defaults(&c->filter);
        c->filter.type = (uint8_t)t;
        c->filter.cutoff = EQ_STEP_1K;
        c->filter.resonance = t & 1 ? 30 : 70;
        c->filter.extra = 30;
        run_case(id++, 0);
    }
    /* Cutoff by CV (the renderer's moving cutoff): +1 V at +0.50 is an octave. */
    filter_value_defaults(&c->filter);
    c->filter.type = FILTER_LP12;
    c->filter.cutoff = EQ_STEP_1K;
    c->filter.source[FILTER_MOD_CUTOFF] = 0;
    c->filter.amount[FILTER_MOD_CUTOFF] = 50;
    cv_volts[0] = 1.0f;
    run_case(id++, 0);
    put("DONE\n");
    for (;;) {
    }
}
#endif
