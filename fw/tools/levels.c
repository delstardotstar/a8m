/* Worst-case output level of machine voices, on the host: the raw voice
 * output (what the engine takes, before the trim, envelope, Bits and level)
 * over every corner of the control space (up to 8 controls), random
 * combinations biased to the extremes and a hill climb from the loudest
 * found. Each case triggers, plays, then retriggers mid-note with new
 * controls and pitch. Pitch-input voices also sweep the channel pitch
 * (-48..+60 st); drum machines' pitch is their own controls (the channel
 * pitch only resamples them).
 * usage: levels VOICE [RANDOM_CASES [SEED]]  (see levels.sh)
 * Prints one tab-separated line: id name worst_peak case default_peak
 * default_rms p50 p95 (raw; peaks over cases; rms = loudest 100 ms), then
 * the gain the level rule gives (default peak 1.0, RMS at most
 * VOICE_RMS_CAP), the voice's gain, its default peak and RMS after gain
 * and voice_clip (dBFS at level 0) and the worst peak into the clipper (dB). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "voices.h"
#include "mdkit_tables.h"

#define RATE    44100
#define CHUNK   117             /* about one engine block at 44.1 kHz */
#define PMIN    -4800
#define PMAX    6000

struct cas {
    uint8_t a[8], b[8];
    int pa, pb;                 /* cents */
    unsigned t1, t2;            /* frames before / after the retrigger */
};

static const struct voice_desc *vd;
static void *state;
static unsigned seed = 1;
static float post;              /* nonzero: play() measures voice_clip(post * x) */

static unsigned rnd(unsigned n)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
}

static uint8_t rnd_val(void)
{
    unsigned r = rnd(10);

    return r < 3 ? 0 : r < 6 ? 127 : (uint8_t)rnd(128);
}

static int rnd_pitch(void)
{
    unsigned r = rnd(10);

    return r < 2 ? PMIN : r < 4 ? PMAX : PMIN + (int)rnd(PMAX - PMIN + 1);
}

static void setup(const uint8_t *ctl, int cents)
{
    for (unsigned i = 0; i < vd->nparams; i++) {
        vd->ops->set_control(state, i, ctl[i]);
    }
    if (vd->flags & VF_PITCH_IN) {
        voice_set_pitch(vd, state, (int32_t)((int64_t)cents * 65536 / 1200));
    }
}

static float play(unsigned frames, float peak, double *rms)
{
    static float buf[CHUNK];
    double win[RATE / 10 / CHUNK + 1] = { 0 }, sum = 0;
    unsigned nw = sizeof(win) / sizeof(win[0]), w = 0;

    for (unsigned n = 0; n < frames; n += CHUNK) {
        double e = 0;
        vd->ops->render(state, buf, CHUNK);
        for (unsigned i = 0; post && i < CHUNK; i++) {
            buf[i] = voice_clip(post * buf[i]);
        }
        for (unsigned i = 0; i < CHUNK; i++) {
            float a = fabsf(buf[i]);
            peak = a > peak ? a : peak;
            e += (double)buf[i] * buf[i];
        }
        if (rms) {
            sum += e - win[w];
            win[w] = e;
            w = (w + 1) % nw;
            double r = sqrt(sum / (nw * CHUNK));
            *rms = r > *rms ? r : *rms;
        }
    }
    return peak;
}

static float run(const struct cas *c)
{
    float peak;

    vd->ops->init(state);
    setup(c->a, c->pa);
    vd->ops->trigger(state);
    peak = play(c->t1, 0.0f, 0);
    setup(c->b, c->pb);
    vd->ops->trigger(state);
    return play(c->t2, peak, 0);
}

static void show(const struct cas *c, char *s)
{
    s += sprintf(s, "[");
    for (unsigned i = 0; i < vd->nparams; i++) {
        s += sprintf(s, "%s%s=%d", i ? " " : "", vd->params[i].name, c->a[i]);
    }
    if (vd->flags & VF_PITCH_IN) {
        s += sprintf(s, " p=%+d", c->pa / 100);
    }
    s += sprintf(s, "] %.2fs ->[", (double)c->t1 / RATE);
    for (unsigned i = 0; i < vd->nparams; i++) {
        s += sprintf(s, "%s%s=%d", i ? " " : "", vd->params[i].name, c->b[i]);
    }
    if (vd->flags & VF_PITCH_IN) {
        s += sprintf(s, " p=%+d", c->pb / 100);
    }
    sprintf(s, "]");
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int id = argc > 1 ? atoi(argv[1]) : 0;
    unsigned nrand = argc > 2 ? (unsigned)atoi(argv[2]) : 2000;
    seed = argc > 3 ? (unsigned)atoi(argv[3]) : 1;

    if (id < 1 || id >= VOICE_COUNT) {
        return 1;
    }
    mdkit_tables_init();
    vd = voices[id];
    state = calloc(1, VOICE_STATE_MAX);

    unsigned n = vd->nparams, ncorner = 1u << n, pitches = vd->flags & VF_PITCH_IN ? 3 : 1;
    unsigned total = ncorner * pitches + nrand, k = 0;
    float *peaks = malloc(total * sizeof(float)), worst = -1.0f;
    struct cas best, c;
    static const int corner_pitch[] = { 0, PMIN, PMAX };

    for (unsigned m = 0; m < ncorner; m++) {
        for (unsigned p = 0; p < pitches; p++) {
            for (unsigned i = 0; i < n; i++) {
                c.a[i] = c.b[i] = (m >> i) & 1 ? 127 : 0;
            }
            c.pa = c.pb = corner_pitch[p];
            c.t1 = RATE / 4;
            c.t2 = RATE * 5 / 4;
            peaks[k++] = run(&c);
            if (peaks[k - 1] > worst) {
                worst = peaks[k - 1];
                best = c;
            }
        }
    }
    for (unsigned r = 0; r < nrand; r++) {
        for (unsigned i = 0; i < n; i++) {
            c.a[i] = rnd_val();
            c.b[i] = rnd(2) ? c.a[i] : rnd_val();
        }
        c.pa = rnd_pitch();
        c.pb = rnd(2) ? c.pa : rnd_pitch();
        c.t1 = RATE / 50 + rnd(RATE * 6 / 10);
        c.t2 = RATE + rnd(RATE / 2);
        peaks[k++] = run(&c);
        if (peaks[k - 1] > worst) {
            worst = peaks[k - 1];
            best = c;
        }
    }

    /* Hill climb: nudge each control (and the pitch) of the loudest case. */
    static const int steps[] = { 32, -32, 8, -8, 2, -2 };
    for (int pass = 0, better = 1; pass < 4 && better; pass++) {
        better = 0;
        for (unsigned i = 0; i <= n; i++) {
            if (i == n && !(vd->flags & VF_PITCH_IN)) {
                break;
            }
            for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
                c = best;
                for (int half = 0; half < 2; half++) {
                    if (i < n) {
                        uint8_t *v = half ? c.b : c.a;
                        int x = v[i] + steps[s];
                        v[i] = (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x);
                    } else {
                        int *p = half ? &c.pb : &c.pa, x = *p + steps[s] * 25;
                        *p = x < PMIN ? PMIN : x > PMAX ? PMAX : x;
                    }
                }
                float pk = run(&c);
                if (pk > worst) {
                    worst = pk;
                    best = c;
                    better = 1;
                }
            }
        }
    }

    uint8_t def[8];
    double rms = 0;
    for (unsigned i = 0; i < n; i++) {
        def[i] = (uint8_t)vd->params[i].def;
    }
    vd->ops->init(state);
    setup(def, 0);
    vd->ops->trigger(state);
    float dpeak = play(RATE * 2, 0.0f, &rms);
    double orms = 0;
    post = vd->gain;
    vd->ops->init(state);
    setup(def, 0);
    vd->ops->trigger(state);
    float opeak = play(RATE * 2, 0.0f, &orms);
    double rule = 1.0 / dpeak;
    rule = rule * rms > VOICE_RMS_CAP ? VOICE_RMS_CAP / rms : rule;

    char s[512];
    qsort(peaks, k, sizeof(float), cmpf);
    show(&best, s);
    printf("%d\t%s\t%.6f\t%s\t%.6f\t%.6f\t%.6f\t%.6f\t%.4f\t%.4f\t%.1f\t%.1f\t%+.1f\n", id, vd->name, (double)worst, s,
           (double)dpeak, rms, (double)peaks[k / 2], (double)peaks[k * 95 / 100], rule, (double)vd->gain,
           20 * log10(opeak), 20 * log10(orms), 20 * log10((double)worst * vd->gain));
    return 0;
}
