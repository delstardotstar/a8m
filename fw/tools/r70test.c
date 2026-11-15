/* Host checks of the 70 (tools/r70test.sh):
 * - the laws against the program data: every program's network and
 *   control values at rest, and every parameter value's network values
 *   (exact, or near where our laws replace the data's tables);
 * - the engine's network (fw/src/r70.c) against literal double models of
 *   the listings, bit for bit, with the detector, the walk and edits
 *   (delay ramps, slews) running: the models read the network values the
 *   engine had in each block of 8;
 * - RT60 of every reverb program against its RT MID;
 * - the walk's depth and speed per modulated program (printed);
 * - stability: loud noise at the stored settings and at the fields'
 *   extremes, then silence: bounded and decaying;
 * - edits under a steady tone: clicks;
 * - the resamplers (the network as a wire): passband, images, aliasing. */
#include "r70.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include REF_C
#include EXPECT_C

#define FS      48000
#define FS70    (13000000.0 / 384.0)
#ifndef BLOCK
#define BLOCK   32
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int32_t send[BLOCK][2], wet[BLOCK][2];
static int only = -1;

static void run(int frames, double (*in)(int))
{
    for (int f0 = 0; f0 < frames; f0 += BLOCK) {
        for (int f = 0; f < BLOCK; f++) {
            send[f][0] = in ? (int32_t)lrint(in(f0 + f) * 2147483647.0) : 0;
        }
        r70_block((const int32_t (*)[2])send, wet, BLOCK);
        r70_poll();
    }
}

static void start(const struct r70_program *p)
{
    r70_load(p);
    do {
        run(BLOCK, 0);
    } while (!r70_ready());
}

static const struct r70_param *param_of(const struct r70_program *p, int role)
{
    const struct r70_algo *a = &r70_algos[p->jalgo];
    for (unsigned i = 0; i < a->nparams; i++) {
        if (a->params[i].role == role) return &a->params[i];
    }
    return 0;
}

/* ---- laws ---- */

/* Our laws replace the data's tables of the unit's memory (r70gen.py): delays
 * from the displayed milliseconds, the SIZE tables from the listings, the
 * fitted ATTACK rows, the signed-percent law of 7.2. Values within these
 * margins of the data count as near; anything further fails. */
static int near(int kind, int got, int want)
{
    int d = abs(got - want);
    return kind ? d <= 6 || d * 50 <= abs(want) : d <= 3;
}

static void check_laws(void)
{
    int bad = 0, nbad = 0, approx = 0, papprox = 0, dmax = 0, cmax = 0;

    for (unsigned i = 0; i < r70_program_count; i++) {
        const struct r70_program *p = &r70_programs[i];
        const struct expect *e = &expects[i];
        struct r70_values v;
        int nb = 0;

        r70_build(p, 0, &v);
        for (int k = 0; k < e->ncoef; k++) {
            if (e->coef[k] == v.coef[k]) continue;
            cmax = abs(v.coef[k] - e->coef[k]) > cmax ? abs(v.coef[k] - e->coef[k]) : cmax;
            if (near(0, v.coef[k], e->coef[k])) approx++;
            else if (nb++ < 4) printf("  %s coef %d: %d, data %d\n", p->name, k, v.coef[k], e->coef[k]);
        }
        for (int k = 0; k < e->ndelay; k++) {
            if (e->delay[k] == v.delay[k]) continue;
            dmax = abs(v.delay[k] - e->delay[k]) > dmax ? abs(v.delay[k] - e->delay[k]) : dmax;
            if (near(1, v.delay[k], e->delay[k])) approx++;
            else if (nb++ < 4) printf("  %s delay %d: %d, data %d\n", p->name, k, v.delay[k], e->delay[k]);
        }
        for (int k = 0; k < R70_CTLS; k++) {
            if (e->ctl[k] != v.ctl[k] && nb++ < 4) printf("  %s control %d: %d, data %d\n", p->name, k, v.ctl[k], e->ctl[k]);
        }
        bad += nb != 0;
    }
    CHECK(!bad, "laws: %d of %u programs differ from the data", bad, r70_program_count);
    for (unsigned j = 0; j < sizeof(pexpects) / sizeof(pexpects[0]); j++) {
        const struct pexpect *x = &pexpects[j];
        struct r70_program q = r70_programs[pbase[x->algo]];
        struct r70_values v;

        q.value[x->param] = x->v;
        q.npatches = 0;
        r70_build(&q, 0, &v);
        int got = x->kind ? v.delay[x->idx] : v.coef[x->idx];
        if (got == x->value) continue;
        if (near(x->kind, got, x->value)) {
            papprox++;
        } else if (nbad++ < 8) {
            printf("  %s %s v %d: %s %d is %d, data %d\n", r70_algos[x->algo].name,
                   r70_algos[x->algo].params[x->param].name, x->v, x->kind ? "delay" : "coef", x->idx, got, x->value);
        }
    }
    CHECK(!nbad, "laws: %d parameter values differ from the data", nbad);
    printf("laws: %u programs, %zu parameter values against the data; near: %d program values (delays within "
           "%d samples, coefficients %d units), %d parameter values\n", r70_program_count,
           sizeof(pexpects) / sizeof(pexpects[0]), approx, dmax, cmax, papprox);
}

/* ---- the network against the literal model ---- */

#define LIT_N   40000
static int16_t lx[LIT_N], ly[LIT_N][2];
static struct r70_run snaps[LIT_N / 8];

static int16_t stim16(int t)
{
    static uint32_t s = 1;
    s = s * 1664525u + 1013904223u;
    int n = (int)(s >> 16) - 32768;
    if (t < 3000) return (int16_t)(n * 3 / 8);
    if (t < 12000) return 0;
    if (t < 12010) return 20000;
    if (t < 16000) return 0;
    if (t < 24000) return (int16_t)(n / 16);
    return 0;
}

static void check_literal(const struct r70_program *p)
{
    const struct r70_algo *a = &r70_algos[p->jalgo];
    double cv[R70_COEFS];
    unsigned dv[R70_DELAYS];
    int bad = 0, first = -1;
    uint8_t f[R70_FIELDS_MAX];
    unsigned nf = r70_fields(p, f);

    start(p);
    for (int t = 0; t < LIT_N; t++) lx[t] = stim16(t);
    /* edits midway: delay ramps and slews run while the model follows */
    r70_native(lx, ly, 20000, snaps, 20000 / 8);
    for (unsigned j = 0; j < nf; j++) {
        int lo, hi;
        r70_edit_range(p, f[j], &lo, &hi);
        if (f[j] != R70_E_SIZE) r70_set(f[j], (r70_edit_base(p, f[j]) + (hi - lo) / 3) % (hi + 1));
    }
    r70_poll();
    r70_native(lx + 20000, ly + 20000, LIT_N - 20000, snaps + 20000 / 8, (LIT_N - 20000) / 8);
    memset(rbuf, 0, sizeof(rbuf));
    rt = 0;
    for (int b = 0; b < LIT_N / 8; b++) {
        const struct r70_run *r = &snaps[b];
        for (int k = 0; k < R70_COEFS; k++) cv[k] = r->sgn[k] ? -(double)r->mag[k] : (double)r->mag[k];
        for (int k = 0; k < R70_DELAYS; k++) dv[k] = r->delay[k];
        for (int i = 0; i < 8; i++) {
            int t = b * 8 + i;
            double L, R;
            ref_net[a->net](cv, dv, r->bypass, 4.0 * lx[t] + 3, &L, &R);
            if (L != ly[t][0] || R != ly[t][1]) {
                if (first < 0) first = t;
                bad++;
            }
        }
    }
    if (bad || only >= 0) {
        printf("  %3u %-12s literal: %d of %d samples differ (first %d)\n", p->number, p->name, bad, LIT_N, first);
    }
    CHECK(!bad, "%u %s: the network differs from the literal model at %d samples", p->number, p->name, bad);
}

/* ---- RT60 ---- */

static double parse_seconds(const char *t)
{
    return strchr(t, 'S') && !strstr(t, "MS") ? atof(t) : -1;
}

/* After the input's fade-in (10 ms). */
static double impulse(int t) { return t >= 1024 && t < 1040 ? 0.5 : 0.0; }

static void check_rt(const struct r70_program *p)
{
    const struct r70_param *q = param_of(p, R70_R_RT_MID), *g = param_of(p, R70_R_GATE);
    struct r70_values v;
    if (!q || r70_algos[p->jalgo].net == R70_NET_INVERSE) return;
    r70_build(p, 0, &v);
    const struct r70_algo *a = &r70_algos[p->jalgo];
    const struct r70_param *ql = param_of(p, R70_R_RT_LOW);
    double want = parse_seconds(r70_text(q, v.eff[q - a->params]));
    double low = ql ? parse_seconds(r70_text(ql, v.eff[ql - a->params])) : want;
    if (want <= 0 || (g && v.ctl[R70_C_GATE])) return;
    double lo_rt = low > 0 ? fmin(want, low) : want, hi_rt = low > 0 ? fmax(want, low) : want;
    int frames = (int)(fmin(want * 2.5, 30.0) * FS) + FS / 2;
    double *e = calloc((size_t)frames / BLOCK + 2, sizeof(double));
    start(p);
    for (int f0 = 0; f0 < frames; f0 += BLOCK) {
        for (int f = 0; f < BLOCK; f++) send[f][0] = (int32_t)lrint(impulse(f0 + f) * 2147483647.0);
        r70_block((const int32_t (*)[2])send, wet, BLOCK);
        for (int f = 0; f < BLOCK; f++) {
            double l = wet[f][0] / 1073741824.0, r = wet[f][1] / 1073741824.0;
            e[f0 / BLOCK] += l * l + r * r;
        }
    }
    int nb = frames / BLOCK;
    for (int i = 0; i + 1024 / BLOCK < nb; i++) e[i] = e[i + 1024 / BLOCK];
    nb -= 1024 / BLOCK;
    for (int i = nb - 2; i >= 0; i--) e[i] += e[i + 1];
    double t5 = -1, t25 = -1;
    for (int i = 0; i < nb; i++) {
        double db = 10 * log10(e[i] / e[0] + 1e-30);
        if (t5 < 0 && db <= -5) t5 = i;
        if (t25 < 0 && db <= -25) t25 = i;
    }
    double got = t25 > 0 ? (t25 - t5) * BLOCK / (double)FS * 3.0 : -1;
    printf("  %3u %-12s RT60 %.2f s, RT MID %.2f s, RT LOW %.2f s\n", p->number, p->name, got, want, low);
    CHECK(got > 0.25 * lo_rt && got < 4 * hi_rt, "%u %s: RT60 %.2f s for %.2f-%.2f s", p->number, p->name, got, lo_rt,
          hi_rt);
    free(e);
}

/* ---- the delay modulation: depth and speed of the walk ---- */

#ifndef WALK_S
#define WALK_S     60
#endif
#define WALK_CHUNK 8192

/* Per program with a running walk: the modulated tap delays' mean, standard
 * deviation and range (samples) and their mean speed (samples/s), averaged
 * over the active modulators. Not checked, printed: compares sequences. */
static void walk_stats(const struct r70_program *p)
{
    static int16_t x[WALK_CHUNK], y[WALK_CHUNK][2];
    static struct r70_run sn[WALK_CHUNK / 8];
    const struct r70_netinfo *ni = &r70_netinfo[r70_algos[p->jalgo].net];
    struct r70_values v;
    r70_build(p, 0, &v);
    unsigned n = ni->nmods == 6 ? (unsigned)(v.ctl[R70_C_CHORUS] & 15) : ni->nmods;
    if (!n || !ni->nmods || !v.ctl[R70_C_CHORUSING]) return;
    double s1[6] = { 0 }, s2[6] = { 0 }, lo[6], hi[6], sp[6] = { 0 }, last[6];
    long cnt = 0;
    for (unsigned j = 0; j < 6; j++) lo[j] = 1e9, hi[j] = -1e9;
    start(p);
    long total = (long)(WALK_S * FS70) / WALK_CHUNK;
    for (long c = 0; c < total; c++) {
        r70_native(x, y, WALK_CHUNK, sn, WALK_CHUNK / 8);
        for (unsigned b = 0; b < WALK_CHUNK / 8; b++, cnt++) {
            for (unsigned j = 0; j < n; j++) {
                const struct r70_mod *m = &ni->mods[j];
                double dA = sn[b].delay[m->A], dB = sn[b].delay[m->B];
                double d = dA + sn[b].mag[m->k] / 256.0 * (dB - dA);
                s1[j] += d;
                s2[j] += d * d;
                lo[j] = fmin(lo[j], d);
                hi[j] = fmax(hi[j], d);
                if (cnt) sp[j] += fabs(d - last[j]);
                last[j] = d;
            }
        }
    }
    double mean = 0, sd = 0, range = 0, speed = 0;
    for (unsigned j = 0; j < n; j++) {
        double m = s1[j] / cnt;
        mean += m / n;
        sd += sqrt(fmax(0, s2[j] / cnt - m * m)) / n;
        range += (hi[j] - lo[j]) / n;
        speed += sp[j] / WALK_S / n;
    }
    printf("  %3u %-12s walk (%u taps, %d s): mean %.1f, sd %.2f, range %.1f samples, speed %.2f samples/s\n",
           p->number, p->name, n, WALK_S, mean, sd, range, speed);
}

/* ---- stability ---- */

static int check_loud(const struct r70_program *p, const uint8_t *e, const int *v, int n, const char *what, int strict)
{
    double early = 0, late = 0, peak = 0;
    int bad = 0;
    struct r70_values vv;

    start(p);
    for (int j = 0; j < n; j++) r70_set(e[j], v[j]);
    r70_poll();
    run(FS / 10, 0);
    srand(2);
    for (int f0 = 0; f0 < 6 * FS; f0 += BLOCK) {
        for (int f = 0; f < BLOCK; f++) send[f][0] = f0 < 3 * FS / 2 ? (int32_t)(rand() * 2u - 0x80000000u) : 0;
        r70_block((const int32_t (*)[2])send, wet, BLOCK);
        r70_poll();
        for (int f = 0; f < BLOCK; f++) {
            double l = wet[f][0] / 1073741824.0, r = wet[f][1] / 1073741824.0, e2 = l * l + r * r;
            bad |= !isfinite(l) || !isfinite(r);
            if (f0 >= 2 * FS && f0 < 3 * FS) early += e2;
            if (f0 >= 5 * FS) late += e2;
            if (f0 >= 5 * FS) peak = fmax(peak, fmax(fabs(l), fabs(r)));
        }
    }
    int16_t ed[R70_EDITS];
    for (int j = 0; j < R70_EDITS; j++) ed[j] = -1;
    for (int j = 0; j < n; j++) ed[e[j]] = (int16_t)v[j];
    r70_build(p, ed, &vv);
    int frozen = vv.ctl[R70_C_RT_MID] >= 253;
    double fall = 10 * log10((late + 1e-20) / (early + 1e-20));
    int ok = !bad && (frozen || (strict ? fall < -1.0 || late < 1e-6 * FS : fall < 0.5 || late < 1e-6 * FS));
    if (!ok || only >= 0) {
        printf("  %3u %-12s %s: after the noise %.1f dB over 3 s, peak %.0f dBFS%s\n", p->number, p->name, what, fall,
               20 * log10(peak + 1e-12), frozen ? " (frozen)" : "");
    }
    CHECK(ok, "%u %s (%s): tail %.1f dB, peak %.3f", p->number, p->name, what, fall, peak);
    return ok;
}

static void check_extremes(const struct r70_program *p)
{
    uint8_t f[R70_FIELDS_MAX];
    unsigned nf = r70_fields(p, f);
    int lo[R70_FIELDS_MAX], hi[R70_FIELDS_MAX];

    /* The voices' own feedback reaches past 1 at the top of its range (the
     * unit's): at the maximum those fields stay as stored. */
    for (unsigned j = 0; j < nf; j++) {
        r70_edit_range(p, f[j], &lo[j], &hi[j]);
        if (f[j] == R70_E_FDBKMST || f[j] == R70_E_RESNMST || f[j] == R70_E_FB1 || f[j] == R70_E_FB2) {
            hi[j] = r70_edit_base(p, f[j]);
        }
    }
    check_loud(p, 0, 0, 0, "as stored", 1);
    check_loud(p, f, hi, (int)nf, "every field at its maximum", 0);
    check_loud(p, f, lo, (int)nf, "every field at its minimum", 0);
}

/* ---- edits ---- */

static double edits_worst = -99, switch_worst = -99;
static int edits_n, edits_over;

static double tone(int t) { return 0.25 * sin(2 * M_PI * 500.0 * t / FS); }

static void check_edit(const struct r70_program *p, unsigned e, int value)
{
    double before = 0, after = 0, y1[2] = { 0, 0 }, y2[2] = { 0, 0 };
    int at = 0;
    char name[16];

    start(p);
    run(FS * 3 / 2, tone);
    for (int f0 = 0; f0 < FS; f0 += BLOCK) {
        if (f0 == FS / 2) {
            r70_set(e, value);
        }
        for (int f = 0; f < BLOCK; f++) send[f][0] = (int32_t)lrint(tone(3 * FS / 2 + f0 + f) * 2147483647.0);
        r70_block((const int32_t (*)[2])send, wet, BLOCK);
        r70_poll();
        for (int f = 0; f < BLOCK; f++) {
            for (int c = 0; c < 2; c++) {
                double y = wet[f][c] / 1073741824.0, d2 = fabs(y - 2 * y1[c] + y2[c]);
                y2[c] = y1[c];
                y1[c] = y;
                if (f0 + f > 100 && f0 < FS / 2) before = fmax(before, d2);
                if (f0 >= FS / 2 && d2 > after) {
                    after = d2;
                    at = f0 + f - FS / 2;
                }
            }
        }
    }
    double ratio = 20 * log10((after + 1e-7) / (before + 1e-7));
    edits_worst = fmax(edits_worst, ratio);
    edits_n++;
    edits_over += ratio >= 12;
    if (only >= 0 || ratio >= 12) {
        printf("  %3u %-12s %s -> %d: clicks %+.1f dB over the steady part (worst %d samples after)\n", p->number,
               p->name, r70_edit_name(p, e, name), value, ratio, at);
    }
    /* Coefficients step in 1/32 (some in 1/8, the unit's resolution): a
     * step of one unit on a full voice is ~-30 dB of it, which reads +15-18
     * dB here. */
    CHECK(ratio < 20, "%u %s: %s clicks %+.1f dB", p->number, p->name, name, ratio);
}

/* A program change under a steady tone, as the page makes it: the old
 * program fades out, the new one starts from silence. */
static void check_switch(const struct r70_program *a, const struct r70_program *b)
{
    double before = 0, change = 0, steady = 0, y1[2] = { 0, 0 }, y2[2] = { 0, 0 };
    const int at = FS / 2, until = at + FS / 10;

    start(a);
    run(FS * 3 / 2, tone);
    for (int f0 = 0; f0 < 2 * FS; f0 += BLOCK) {
        if (f0 == at) {
            r70_load(b);
        }
        for (int f = 0; f < BLOCK; f++) send[f][0] = (int32_t)lrint(tone(3 * FS / 2 + f0 + f) * 2147483647.0);
        r70_block((const int32_t (*)[2])send, wet, BLOCK);
        r70_poll();
        for (int f = 0; f < BLOCK; f++) {
            int t = f0 + f;
            for (int c = 0; c < 2; c++) {
                double y = wet[f][c] / 1073741824.0, d2 = fabs(y - 2 * y1[c] + y2[c]);
                y2[c] = y1[c];
                y1[c] = y;
                if (t > 100 && t < at) before = fmax(before, d2);
                if (t >= at && t < until) change = fmax(change, d2);
                if (t >= until + FS / 2) steady = fmax(steady, d2);
            }
        }
    }
    /* the change (fade out, silence, fade in) against either program's steady sound */
    double ratio = 20 * log10((change + 1e-7) / (fmax(before, steady) + 1e-7));
    switch_worst = fmax(switch_worst, ratio);
    CHECK(ratio < 12, "%u %s -> %u %s: clicks %+.1f dB", a->number, a->name, b->number, b->name, ratio);
}

/* ---- resamplers ---- */

static double sine_f, sine_a = 0.5;
static double sine(int t) { return sine_a * sin(2 * M_PI * sine_f * t / FS); }

/* The level at the bus (dB re the input) of frequency f in the output
 * for a sine at fin, by a Goertzel over 1 s. */
static double through(double fin, double fout)
{
    double s1[2] = { 0 }, s2[2] = { 0 }, w = 2 * cos(2 * M_PI * fout / FS), pw = 0;
    int n = FS;

    sine_f = fin;
    r70_identity(1);
    start(&r70_programs[0]);
    run(FS / 4, sine);
    for (int f0 = 0; f0 < n; f0 += BLOCK) {
        for (int f = 0; f < BLOCK; f++) send[f][0] = (int32_t)lrint(sine(FS / 4 + f0 + f) * 2147483647.0);
        r70_block((const int32_t (*)[2])send, wet, BLOCK);
        for (int f = 0; f < BLOCK; f++) {
            double y = wet[f][0] / 1073741824.0, s0 = y + w * s1[0] - s2[0];
            s2[0] = s1[0];
            s1[0] = s0;
        }
    }
    pw = s1[0] * s1[0] + s2[0] * s2[0] - w * s1[0] * s2[0];
    r70_identity(0);
    double amp = 2 * sqrt(pw) / n;
    return 20 * log10(amp / sine_a + 1e-12);
}

static void check_resampler(void)
{
    struct r70_values v;
    r70_build(&r70_programs[0], 0, &v);
    double g0 = 20 * log10(v.fxgain), ref = through(1000, 1000) - g0, lo = 0, hi = -99, worst_img = -999,
           worst_alias = -999;

    for (double f = 50; f <= 15000; f *= 1.25) {
        double d = through(f, f) - g0 - ref;
        lo = fmin(lo, d);
        hi = fmax(hi, d);
    }
    double at15 = through(15000, 15000) - g0 - ref, at16 = through(16500, 16500) - g0 - ref;
    /* images of the unit's rate: fs70 - f for tones up to 15 kHz */
    for (double f = 1000; f <= 15000; f += 2000) {
        worst_img = fmax(worst_img, through(f, FS70 - f) - g0);
    }
    /* aliases into the band: input tones above the unit's Nyquist */
    for (double f = 19000; f <= 23500; f += 1500) {
        worst_alias = fmax(worst_alias, through(f, FS70 - f) - g0);
    }
    printf("resampler: gain at 1 kHz %+.3f dB; 50 Hz-15 kHz %+.3f..%+.3f dB; 15 kHz %+.2f, 16.5 kHz %+.2f dB\n", ref,
           lo, hi, at15, at16);
    printf("resampler: images (fs70 - f, f 1-15 kHz) worst %.1f dB; aliases (19-23.5 kHz in) worst %.1f dB\n",
           worst_img, worst_alias);
    CHECK(fabs(ref) < 0.1 && lo > -0.5 && hi < 0.5, "resampler passband %+.2f..%+.2f dB", lo, hi);
    CHECK(worst_img < -60 && worst_alias < -60, "resampler images %.1f, aliases %.1f dB", worst_img, worst_alias);
}

int main(int argc, char **argv)
{
    if (argc > 1) only = atoi(argv[1]);
    if (only < 0) {
        check_laws();
        check_resampler();
    }
    int per_algo[R70_ALGOS] = { 0 };
    for (unsigned i = 0; i < r70_program_count; i++) {
        const struct r70_program *p = &r70_programs[i];
        if (only >= 0 && p->number != only) continue;
        check_literal(p);
        check_rt(p);
        walk_stats(p);
        check_extremes(p);
        if (per_algo[p->algo]++ < 2 || only >= 0) {
            uint8_t f[R70_FIELDS_MAX];
            unsigned nf = r70_fields(p, f);
            for (unsigned j = 0; j < nf; j++) {
                int lo, hi, b = r70_edit_base(p, f[j]);
                r70_edit_range(p, f[j], &lo, &hi);
                int step = (hi - lo) / 6 > 1 ? (hi - lo) / 6 : 1, down = b > (lo + hi) / 2;
                /* feedback: a small move toward less (both signs feed back) */
                if (f[j] == R70_E_FDBKMST || f[j] == R70_E_RESNMST || f[j] == R70_E_FB1 || f[j] == R70_E_FB2) {
                    step = (hi - lo) / 20 > 1 ? (hi - lo) / 20 : 1;
                    down = b - step >= lo;
                }
                check_edit(p, f[j], down ? b - step : b + step);
            }
        }
    }
    if (only < 0) {
        for (unsigned i = 0; i + 1 < r70_program_count; i += 7) {
            check_switch(&r70_programs[i], &r70_programs[(i * 13 + 5) % r70_program_count]);
        }
        printf("program changes: worst %+.1f dB\n", switch_worst);
    }
    printf("edits: %d, worst %+.1f dB, %d at +12 dB or more\n", edits_n, edits_worst, edits_over);
    printf(fails ? "r70test: %d FAILED\n" : "r70test: ok\n", fails);
    r70_load(0);
    return fails != 0;
}
