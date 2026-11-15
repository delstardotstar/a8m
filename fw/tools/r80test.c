/* Host checks of the 80's reverbs (tools/r80test.sh):
 * - the laws against the program data: every program's tank k and A,
 *   input/output values and parameters in effect;
 * - the engine (fw/src/r80.c) against literal double models of the
 *   spec's listings (tanks, sections 3 and 3.1), motion held;
 * - every program's impulse response with its motion running: RT60
 *   against the literal model's and against its settings, decay, stereo;
 * - stability: loud noise at the program's settings and at parameter
 *   extremes, then silence: bounded, decaying;
 * - edits: a Size / Mid Rt change under a steady tone, clicks measured. */
#include "r80.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include REF_C
#include EXPECT_C

#define FS      48000
#ifndef BLOCK
#define BLOCK   32
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int32_t send[BLOCK][2], wet[BLOCK][2];
static int only = -1;

/* r80.c, host only: io, gates and delays at their targets now. */
void r80_settle(void);

static void run(int frames, double (*in)(int))
{
    for (int f0 = 0; f0 < frames; f0 += BLOCK) {
        for (int f = 0; f < BLOCK; f++) {
            send[f][0] = in ? (int32_t)lrint(in(f0 + f) * 2147483647.0) : 0;
        }
        r80_block((const int32_t (*)[2])send, wet, BLOCK);
        r80_poll();
    }
}

/* Loads p and runs silence until its memory has cleared. */
static void start(const struct r80_program *p)
{
    r80_load(p);
    do {
        run(BLOCK, 0);
    } while (!r80_ready());
}

/* ---- the laws against the program data ---- */

/* Our laws replace the spec's tables of the unit's data (r80gen.py): RT,
 * High Cut and glide scales, Depth, Shape / Attack curves, the Inverse's
 * slope J and listed taps. Values within these margins of the data count
 * as near; anything further fails. */
#define NEAR_K  32      /* tank coefficients, 1/256 (two Shape steps) */
#define NEAR_A  1       /* tank offsets, samples */
#define NEAR_IO 0.06    /* input/output words, relative */

static void check_laws(void)
{
    int bad = 0, approx = 0, near = 0, kmax = 0, amax = 0;
    double iomax = 0;

    for (unsigned i = 0; i < r80_program_count; i++) {
        const struct r80_program *p = &r80_programs[i];
        const struct expect *e = &expects[i];
        struct r80_values s, v;
        struct r80_target t;
        int16_t src[R80_SOURCES];

        for (int j = 0; j < R80_SOURCES; j++) src[j] = 0;
        src[R80_SRC_NONE] = src[R80_SRC_NOVALUE] = -1;
        src[R80_SRC_ON] = 127;
        src[R80_SRC_ADJUST] = p->adjust;
        r80_stored(p, &s);
        r80_patch_all(p, &s, src, &v);
        /* Known departures: BowedChords' data has its AR envelope at the top
         * (7.5); Pedal Swell's footpedal has no value here (16.1: no pedal
         * passes the input), the data reads it as 0; Tempo Verb's and Tempo
         * Gate's tanks follow a Tempo source value their parameters don't. */
        int skip_io = p->number == 139 || p->number == 141, skip_tank = p->number == 70 || p->number == 71;
        for (int q = 0; q < R80_PARAMS; q++) {
            if (e->has_param[q] && !r80_locked(&v, q) && fabs(v.v[q] / 32.0 - e->param[q]) > 1e-9 &&
                p->number != 141) {
                printf("  %3d %-12s parameter %d: %g, data %g\n", p->number, p->name, q, v.v[q] / 32.0, e->param[q]);
                bad++;
            }
        }
        r80_resolve(p, &v, &t, 1);
        for (int k = 0; k < R80_K && !skip_tank; k++) {
            if (!e->has_k[k] || (p->algo == R80_HALL && (k == 1 || k == 7 || k == 115 || k == 121))) {
                continue;
            }
            int want = (int)lrint(e->k[k] * 256), d = t.k[k] - want;
            /* Low Rt's law is within 1.2/256 (4.3, 8); Rt HC 48 ("Full", a = 1)
             * reads as 0 in the data's 8-bit word. */
            int low = (p->algo == R80_CHAMBER && (k == 32 || k == 122)) ||
                      ((p->algo == R80_PLATE || p->algo == R80_CHORUS) && (k == 26 || k == 120)) ||
                      (p->algo == R80_HALL && (k == 29 || k == 73));
            int full = t.k[k] == 256 && want == 0;
            if (d && ((low && abs(d) <= 1) || full)) {
                approx++;
            } else if (d && abs(d) <= NEAR_K) {
                near++;
                kmax = abs(d) > kmax ? abs(d) : kmax;
            } else if (d) {
                printf("  %3d %-12s k%d: %d/256, data %d/256\n", p->number, p->name, k, t.k[k], want);
                bad++;
            }
        }
        for (int a = 0; a < R80_K && !skip_tank; a++) {
            int tap = p->algo == R80_HALL && (a == 3 || a == 4 || a == 109 || a == 112 || a == 117 || a == 118 ||
                                              a == 123 || a == 124);
            if (e->has_a[a] && !tap && t.a[a] != e->a[a] && abs(t.a[a] - e->a[a]) <= NEAR_A) {
                near++;
                amax = abs(t.a[a] - e->a[a]) > amax ? abs(t.a[a] - e->a[a]) : amax;
            } else if (e->has_a[a] && !tap && t.a[a] != e->a[a]) {
                printf("  %3d %-12s A%d: %d, data %d\n", p->number, p->name, a, t.a[a], e->a[a]);
                bad++;
            }
        }
        int n = p->algo == R80_CHORUS ? R80_IO31_COUNT : R80_IO3_COUNT;
        for (int j = 0; j < n && !skip_io; j++) {
            /* The data's 16-bit words (section 11) against the real laws. */
            double tol = 2e-4 * (fabs(e->io[j]) > 1 ? fabs(e->io[j]) : 1);
            double rel = fabs(t.io[j] - e->io[j]) / (fabs(e->io[j]) > 1e-9 ? fabs(e->io[j]) : 1);
            if (fabs(t.io[j] - e->io[j]) > tol && rel <= NEAR_IO) {
                near++;
                iomax = rel > iomax ? rel : iomax;
            } else if (fabs(t.io[j] - e->io[j]) > tol) {
                printf("  %3d %-12s %s: %.7g, data %.7g\n", p->number, p->name,
                       p->algo == R80_CHORUS ? expect_io31[j] : expect_io3[j], t.io[j], e->io[j]);
                bad++;
            }
        }
    }
    printf("laws: %u programs against the program data, %d mismatches, %d within Low Rt's 1/256 or Full; near: %d "
           "(k within %d/256, A %d, io %.1f %%)\n", r80_program_count, bad, approx, near, kmax, amax, 100 * iomax);
    CHECK(!bad, "%d law mismatches", bad);
}

/* ---- the literal model: tank + sections 3 / 3.1 in double ---- */

static struct {
    double post[65536][2], voice[65536][2], diff[4][2048];
    double ret[2], fb[2], s[2], send[2];
    /* 3.1 */
    double la[2], v[6], b[6], m[6], a[6];
    int32_t phi[6], r[6];
    uint32_t u, ncount, kv, ks;
    unsigned n;
} R;

static double tap(double (*line)[2], int side, double d)
{
    unsigned i = (unsigned)d;
    double f = d - i, a = line[(R.n - i) & 65535u][side], b = line[(R.n - i - 1u) & 65535u][side];

    return (1 - f) * a + f * b;
}

static void ref_tank(int algo, double sl, double sr, double *rl, double *rr, const double *k, const int *A)
{
    switch (algo) {
    case R80_CHAMBER: ref_chamber(sl, sr, rl, rr, k, A); break;
    case R80_HALL: ref_hall(sl, sr, rl, rr, k, A); break;
    case R80_INVERSE: ref_inverse(sl, sr, rl, rr, k, A); break;
    default: ref_plate(sl, sr, rl, rr, k, A); break;
    }
}

static int32_t sx24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }

static void ref_sample(int algo, const double *io, double in, const double *k, const int *A, double out[2])
{
    static const unsigned dl[4] = { 741, 1995, 1083, 1653 };
    double ret[2];

    if (algo == R80_CHORUS) {
        double sr = io[R80_C_94] * (io[R80_C_90] + io[R80_C_92]) * in;
        double sl = io[R80_C_94] * (io[R80_C_91] + io[R80_C_93]) * in;
        ref_tank(algo, R.send[0], R.send[1], &ret[0], &ret[1], k, A);
        R.send[0] = sl;
        R.send[1] = sr;
        double fa = 2 * sl, fb = 2 * sr;
        for (int j = 0; j < 3; j++) {
            fa += io[R80_C_FB1 + j] * R.v[j];
            fb += io[R80_C_FB1 + 3 + j] * R.v[3 + j];
        }
        R.la[0] = clip(R.la[0] + io[R80_C_DAMP] * (fa - R.la[0]));
        R.la[1] = clip(R.la[1] + io[R80_C_DAMP] * (fb - R.la[1]));
        R.voice[R.n & 65535u][0] = R.la[0];
        R.voice[R.n & 65535u][1] = R.la[1];
        double ch[2] = { 0, 0 };
        for (int j = 0; j < 6; j++) {
            double d = (floor(R.b[j] + 0.5) + floor(R.m[j] + 0.5)) / 128.0 - 1.0;
            R.v[j] = tap(R.voice, j >= 3, d);
            ch[0] += io[R80_C_CL1 + j] * R.v[j];
            ch[1] += io[R80_C_CR1 + j] * R.v[j];
        }
        double o[2] = { io[R80_C_AC] * ch[0] + io[R80_C_AD] * R.ret[0], io[R80_C_AC] * ch[1] + io[R80_C_AD] * R.ret[1] };
        R.ret[0] = ret[0];
        R.ret[1] = ret[1];
        out[0] = clip(io[R80_C_OUT] * (io[R80_C_AE] * o[0] + io[R80_C_AF] * o[1]));
        out[1] = clip(io[R80_C_OUT] * (io[R80_C_AE] * o[1] + io[R80_C_AF] * o[0]));
        for (int j = 0; j < 6; j++) {
            R.b[j] += io[R80_C_GLIDE] * (io[R80_C_B1 + j] - R.b[j]);
            R.m[j] = fabs(R.a[j] / 8388608.0 * R.phi[j]);
            R.phi[j] = sx24(R.phi[j] + R.r[j]);
        }
        if (--R.ncount == 0) {
            int32_t su = sx24((int32_t)R.u), au = su < 0 ? -su : su;
            double rr = io[R80_C_R1 + R.kv];
            R.r[R.kv] = (int32_t)lrint(((au & 2) ? rr : -rr) + au / 67108864.0 * rr);
            R.ncount = 4096u + ((uint32_t)au >> 11);
            R.kv = (R.kv + 1) % 6;
        }
        R.a[R.ks] += io[R80_C_38] * (io[R80_C_A1 + R.ks] - R.a[R.ks]);
        R.ks = (R.ks + 1) % 6;
        R.u = (R.u + (uint32_t)io[R80_C_U]) & 0xffffffu;
        R.n++;
        return;
    }
    double m[2] = { (io[R80_IO_ALL] + io[R80_IO_ALR]) * in, (io[R80_IO_ARL] + io[R80_IO_ARR]) * in };
    ref_tank(algo, R.send[0], R.send[1], &ret[0], &ret[1], k, A);
    R.send[0] = io[R80_IO_GS] * m[0];
    R.send[1] = io[R80_IO_GS] * m[1];
    double r[2] = { io[R80_IO_GR] * R.ret[0], io[R80_IO_GR] * R.ret[1] };
    R.ret[0] = ret[0];
    R.ret[1] = ret[1];
    for (int s = 0; s < 2; s++) R.post[R.n & 65535u][s] = clip(r[s]);
    double t[2] = { tap(R.post, 0, io[R80_IO_POSTL]), tap(R.post, 1, io[R80_IO_POSTR]) };
    double p[2] = { io[R80_IO_PD] * t[0] + io[R80_IO_PR] * r[0], io[R80_IO_PD] * t[1] + io[R80_IO_PR] * r[1] };
    double q[2] = { io[R80_IO_C1] * p[0] + io[R80_IO_C2] * p[1], io[R80_IO_C1] * p[1] + io[R80_IO_C2] * p[0] };
    double bus[2] = { 0, 0 };
    if (io[R80_IO_FV] != 0) {
        double v[2];
        for (int s = 0; s < 2; s++) {
            double x = m[s];
            if (io[R80_IO_VD] != 0) {
                for (int j = 0; j < 2; j++) {
                    double *line = R.diff[2 * s + j], d = line[(R.n - dl[2 * s + j]) & 2047u];
                    double w = clip(x - io[R80_IO_G] * d);
                    line[R.n & 2047u] = w;
                    x = d + io[R80_IO_G] * w;
                }
            }
            v[s] = clip(io[R80_IO_VD] * x + io[R80_IO_VP] * m[s] + R.fb[s]);
            R.voice[R.n & 65535u][s] = v[s];
        }
        const double *g = &io[R80_IO_V1L];
        double t1 = tap(R.voice, 0, io[R80_IO_DV1]), t2 = tap(R.voice, 0, io[R80_IO_DV1 + 1]);
        double t3 = tap(R.voice, 1, io[R80_IO_DV1 + 2]), t4 = tap(R.voice, 1, io[R80_IO_DV1 + 3]);
        bus[0] = t1 * g[0] + t2 * g[3] + t3 * g[6] + t4 * g[9];
        bus[1] = t1 * g[1] + t2 * g[4] + t3 * g[7] + t4 * g[10];
        R.fb[0] = clip(g[2] * t1 + g[5] * t2);
        R.fb[1] = clip(g[8] * t3 + g[11] * t4);
    }
    for (int s = 0; s < 2; s++) {
        R.s[s] = clip(R.s[s] + io[R80_IO_A] * (io[R80_IO_FV] * bus[s] + io[R80_IO_FR] * q[s] - R.s[s]));
    }
    out[0] = clip(io[R80_IO_OUT] * (io[R80_IO_W1] * R.s[0] + io[R80_IO_W2] * R.s[1]));
    out[1] = clip(io[R80_IO_OUT] * (io[R80_IO_W1] * R.s[1] + io[R80_IO_W2] * R.s[0]));
    R.n++;
}

/* RT60 from the time the Schroeder decay of a stereo response takes
 * from -5 dB to -5 - range dB, less a floor power. */
static double rt60(const float *y, int frames, double floor, int range)
{
    double *edc = malloc(sizeof(double) * frames), sum = 0;
    int t5 = -1, t35 = -1;

    for (int i = frames - 1; i >= 0; i--) {
        double e = (double)y[2 * i] * y[2 * i] + (double)y[2 * i + 1] * y[2 * i + 1] - floor;
        sum += e > 0 ? e : 0;
        edc[i] = sum;
    }
    for (int i = 0; i < frames; i++) {
        double db = 10 * log10(edc[i] / edc[0] + 1e-30);
        if (t5 < 0 && db <= -5) t5 = i;
        if (t35 < 0 && db <= -5 - range) t35 = i;
    }
    free(edc);
    return t35 > 0 && t5 >= 0 ? 60.0 / range * (t35 - t5) / FS : 0;
}

static double literal_rt[256];

/* The stimulus of the literal check: an impulse, then from 2 s a burst of noise. */
static double stim(int t)
{
    return t == 0 ? 0.5 : (t >= 2 * FS && t < 2 * FS + 4800) ? (rand() / (double)RAND_MAX - 0.5) * 0.6 : 0.0;
}

static double snr_worst = 1e9;

static void check_literal(const struct r80_program *p)
{
    double k[R80_K], e[2] = { 0, 0 }, s[2] = { 0, 0 };
    int A[R80_K];
    const int frames = 4 * FS;
    float *ref = malloc(sizeof(float) * frames * 2);

    r80_hold(1, 1);
    start(p);
    if (getenv("R80_EDIT")) {
        int e, v;
        if (sscanf(getenv("R80_EDIT"), "%d=%d", &e, &v) == 2) {
            r80_set((unsigned)e, v);
            run(FS, 0);
        }
    }
    r80_settle();
    const struct r80_target *t = r80_now();
    for (int i = 0; i < R80_K; i++) {
        A[i] = t->a[i];
    }
    extern const int16_t *r80_k_now(void);
    const int16_t *kn = r80_k_now();
    for (int i = 0; i < R80_K; i++) {
        k[i] = kn[i] / 256.0;
    }
    if (p->algo == R80_HALL) {
        static const int frac[4] = { 115, 121, 1, 7 }, ra[4] = { 109, 117, 123, 3 }, rb[4] = { 112, 118, 124, 4 };
        for (unsigned j = 0; j < 4; j++) {
            unsigned a, b, code;
            r80_taps(j, &a, &b, &code);
            A[ra[j]] = (int)a;
            A[rb[j]] = (int)b;
            k[frac[j]] = (255 - (int)code) / 256.0;
        }
    }
    memset(&R, 0, sizeof(R));
    memset(M, 0, sizeof(M));
    memset(xp, 0, sizeof(xp));
    n = 0;
    double io[R80_IO_MAX];
    for (int j = 0; j < R80_IO_MAX; j++) io[j] = t->io[j];
    if (p->algo == R80_CHORUS) {
        for (int j = 0; j < 6; j++) {
            R.b[j] = io[R80_C_B1 + j];
            R.a[j] = io[R80_C_A1 + j];
            R.r[j] = (int32_t)io[R80_C_R1 + j];
        }
        R.u = (uint32_t)io[R80_C_U0];
        R.ncount = (uint32_t)io[R80_C_N];
    }
    extern void r80_restart(void);
    r80_restart();
    srand(1);
    for (int f0 = 0; f0 < frames; f0 += BLOCK) {
        double in[BLOCK];
        for (int f = 0; f < BLOCK; f++) {
            in[f] = stim(f0 + f);
            send[f][0] = (int32_t)lrint(in[f] * 2147483648.0);
        }
        r80_block((const int32_t (*)[2])send, wet, BLOCK);
        for (int f = 0; f < BLOCK; f++) {
            double o[2];
            ref_sample(p->algo, io, in[f], k, A, o);
            for (int c = 0; c < 2; c++) {
                double y = wet[f][c] / 1073741824.0;
                static int shown;
                if (getenv("R80_DIFF") && shown < 12 && fabs(y - o[c]) > 1e-3) {
                    shown++;
                    printf("    frame %d side %d: engine %.6f model %.6f\n", f0 + f, c, y, o[c]);
                }
                e[c] += (y - o[c]) * (y - o[c]);
                s[c] += o[c] * o[c];
                ref[2 * (f0 + f) + c] = (float)o[c];
            }
        }
    }
    double snr = 1e9;
    for (int c = 0; c < 2; c++) {
        double v = 10 * log10(s[c] / (e[c] + 1e-30));
        snr = v < snr ? v : snr;
    }
    /* Programs whose patches at rest leave the input far down (an input
     * level swelled by an envelope or switch) sit near the 18-bit memory's
     * floor: no check. */
    double level = 10 * log10((s[0] + s[1]) / (2.0 * frames) + 1e-30);
    int quiet = level < -70;
    if (!quiet) {
        snr_worst = snr < snr_worst ? snr : snr_worst;
    }
    literal_rt[p->number] = quiet ? 0 : rt60(ref, 2 * FS, 0, 20);
    printf("  %3u %-12s literal: %.1f dB, RT60 %.2f s (T20), level %.0f dB%s\n", p->number, p->name, snr,
           literal_rt[p->number], level, quiet ? " (quiet at rest: not checked)" : "");
    CHECK(quiet || snr > 40, "%u %s: %.1f dB from the literal model", p->number, p->name, snr);
    free(ref);
    r80_hold(0, 0);
}

/* What the settings give for the decay (4.2): RT[Mid Rt], with Link x
 * (16 + Size) / 160 (Plate, Chamber), Concert Hall about 0.7 x. */
static double rt_expected(const struct r80_program *p)
{
    int mid = r80_live(R80_E_MIDRT), size = r80_live(R80_E_SIZE);
    double rt = r80_rt_ms[mid < 0 ? 0 : mid] / 1000.0;
    struct r80_values v;

    r80_stored(p, &v);
    if (p->algo == R80_HALL) {
        return 0.7 * rt;
    }
    return v.v[R80_P_LINK] ? rt * (16 + size) / 160.0 : rt;
}

/* The impulse response with the motion running. */
static void check_decay(const struct r80_program *p)
{
    const int frames = 4 * FS;
    float *y = malloc(sizeof(float) * frames * 2);
    unsigned lo[4] = { 999, 999, 999, 999 }, hi[4] = { 0, 0, 0, 0 }, apart = 0;

    if (getenv("R80_HOLD")) {
        r80_hold(1, 1);
    }
    start(p);
    run(3 * FS, 0);
    if (getenv("R80_SETTLE")) {
        extern void r80_restart(void);
        r80_settle();
        r80_restart();
    }
    for (int f0 = 0; f0 < frames; f0 += BLOCK) {
        memset(send, 0, sizeof(send));
        if (!f0) send[0][0] = 1 << 30;
        r80_block((const int32_t (*)[2])send, wet, BLOCK);
        r80_poll();
        for (int f = 0; f < BLOCK; f++) {
            y[2 * (f0 + f)] = (float)(wet[f][0] / 1073741824.0);
            y[2 * (f0 + f) + 1] = (float)(wet[f][1] / 1073741824.0);
        }
        if (p->algo == R80_HALL) {
            for (unsigned j = 0; j < 4; j++) {
                unsigned a, b, code, base = 256 + 512 * j;
                r80_taps(j, &a, &b, &code);
                apart |= (a > b ? a - b : b - a) != 1 || a < base || a > base + 255 || b < base || b > base + 255;
                lo[j] = b - base < lo[j] ? b - base : lo[j];
                hi[j] = b - base > hi[j] ? b - base : hi[j];
            }
        }
    }
    if (p->algo == R80_HALL) {
        CHECK(!apart, "%u: a tap's reads left their window or each other", p->number);
    }
    double el = 0, er = 0, lr = 0;
    for (int i = FS / 10; i < FS; i++) {
        el += (double)y[2 * i] * y[2 * i];
        er += (double)y[2 * i + 1] * y[2 * i + 1];
        lr += (double)y[2 * i] * y[2 * i + 1];
    }
    double rt = rt60(y, 2 * FS, 0, 20), corr = lr / sqrt(el * er + 1e-30), want = rt_expected(p);
    double lit = literal_rt[p->number];
    printf("  %3u %-12s RT60 %.2f s (literal %.2f, settings %.2f), L/R %.1f dB, corr %.2f\n", p->number, p->name, rt,
           lit, want, 10 * log10((el + 1e-30) / (er + 1e-30)), corr);
    /* Programs whose modulators move their parameters decay differently
     * running than held at rest: only the static ones are held to it. */
    int moving = 0;
    for (unsigned j = 0; j < p->npatches; j++) {
        unsigned src = p->patch[j].source;
        moving |= src != R80_SRC_ADJUST && src != R80_SRC_NOVALUE && src != R80_SRC_NONE && src != R80_SRC_OFF &&
                  src != R80_SRC_ON;
    }
    /* Chorus+Rvb's modulated voice feedback makes its decay depend on the
     * LFOs' phases. */
    moving |= p->algo == R80_CHORUS;
    if (p->algo != R80_INVERSE && lit > 0.05 && !moving) {
        CHECK(fabs(rt / lit - 1) < 0.2, "%u %s: RT60 %.2f s, literal %.2f s", p->number, p->name, rt, lit);
    }
    if (p->algo != R80_INVERSE && want > 0.25 && want < 3.0 && lit > 0.05 && !moving) {
        CHECK(lit > 0.1 * want && lit < 4 * want, "%u %s: RT60 %.2f s for settings %.2f s", p->number, p->name, lit,
              want);
    }
    free(y);
}

/* Full-scale noise for 1.5 s then silence: never stuck at full scale, and
 * the tail falls. Edits e[] (n of them) to values v[] first. */
static int check_loud(const struct r80_program *p, const unsigned *e, const int *v, int n, const char *what,
                      int strict)
{
    double early = 0, late = 0, peak = 0;
    int bad = 0;

    start(p);
    for (int j = 0; j < n; j++) {
        r80_set(e[j], v[j]);
    }
    run(FS / 10, 0);
    srand(2);
    for (int f0 = 0; f0 < 6 * FS; f0 += BLOCK) {
        for (int f = 0; f < BLOCK; f++) send[f][0] = f0 < 3 * FS / 2 ? (int32_t)(rand() * 2u - 0x80000000u) : 0;
        r80_block((const int32_t (*)[2])send, wet, BLOCK);
        r80_poll();
        for (int f = 0; f < BLOCK; f++) {
            double l = wet[f][0] / 1073741824.0, r = wet[f][1] / 1073741824.0, e2 = l * l + r * r;
            bad |= !isfinite(l) || !isfinite(r);
            if (f0 >= 2 * FS && f0 < 3 * FS) early += e2;
            if (f0 >= 5 * FS) late += e2;
            if (f0 >= 5 * FS) peak = fabs(l) > peak ? fabs(l) : fabs(r) > peak ? fabs(r) : peak;
        }
    }
    double fall = 10 * log10((late + 1e-20) / (early + 1e-20));
    /* At the extremes the program's own gains (voice feedback, output
     * gain) can hold the output at full scale for a while: it must not
     * grow there. */
    int ok = !bad && (strict ? peak < 0.99 && (fall < -1.0 || late < 1e-6 * FS) : fall < 0.0 || late < 1e-6 * FS);
    if (!ok || only >= 0) {
        printf("  %3u %-12s %s: after the noise %.1f dB over 3 s, peak %.0f dBFS\n", p->number, p->name, what, fall,
               20 * log10(peak + 1e-12));
    }
    CHECK(ok, "%u %s (%s): tail %.1f dB, peak %.3f", p->number, p->name, what, fall, peak);
    return ok;
}

static void check_extremes(const struct r80_program *p)
{
    uint8_t f[R80_FIELDS_MAX];
    unsigned nf = r80_fields(p, f), e[R80_FIELDS_MAX];
    int lo[R80_FIELDS_MAX], hi[R80_FIELDS_MAX];

    for (unsigned j = 0; j < nf; j++) {
        e[j] = f[j];
        r80_edit_range(p, f[j], &lo[j], &hi[j]);
        /* Mid Rt 63 is a 65 s decay and the Feedback Master at 100 lets the
         * chorus voices' feedback sum past 1 (the unit's own ranges): the
         * maxima stop short of those. */
        if (f[j] == R80_E_MIDRT) hi[j] = 50;
        if (f[j] == R80_E_FBMASTER) hi[j] = 50;
    }
    check_loud(p, 0, 0, 0, "as stored", 1);
    check_loud(p, e, hi, (int)nf, "every field at its maximum", 0);
    check_loud(p, e, lo, (int)nf, "every field at its minimum", 0);
    /* The longest, least damped decay with the most diffusion. */
    unsigned ex[4] = { R80_E_MIDRT, R80_E_RTHC, R80_E_LOWRT, R80_E_DIFFUSION };
    int vx[4] = { 50, 48, 9, 100 };
    check_loud(p, ex, vx, 4, "Mid Rt 50, Rt HC full, Low Rt x4", 1);
}

/* A 500 Hz tone; edits mid-run: the worst second difference near the
 * change against the steady part's. */
static double tone(int t) { return 0.25 * sin(2 * M_PI * 500.0 * t / FS); }

static void check_edit(const struct r80_program *p, unsigned e, int value, const char *what)
{
    double before = 0, after = 0, y1[2] = { 0, 0 }, y2[2] = { 0, 0 };
    int at = 0;

    start(p);
    run(2 * FS, tone);
    for (int f0 = 0; f0 < FS; f0 += BLOCK) {
        if (f0 == FS / 2) {
            r80_set(e, value);
        }
        for (int f = 0; f < BLOCK; f++) send[f][0] = (int32_t)lrint(tone(2 * FS + f0 + f) * 2147483647.0);
        r80_block((const int32_t (*)[2])send, wet, BLOCK);
        r80_poll();
        for (int f = 0; f < BLOCK; f++) {
            for (int c = 0; c < 2; c++) {
                double y = wet[f][c] / 1073741824.0, d2 = fabs(y - 2 * y1[c] + y2[c]);
                y2[c] = y1[c];
                y1[c] = y;
                if (f0 + f > 100 && f0 < FS / 2) before = d2 > before ? d2 : before;
                if (f0 >= FS / 2 && d2 > after) {
                    after = d2;
                    at = f0 + f - FS / 2;
                }
            }
        }
    }
    double ratio = 20 * log10((after + 1e-9) / (before + 1e-9));
    printf("  %3u %-12s %s: clicks %+.1f dB over the steady part (worst %d samples after)\n", p->number, p->name,
           what, ratio, at);
    /* A Size or Duration change restarts the tank: its build-up again. */
    int restart = e == R80_E_SIZE || e == R80_E_DURATION;
    CHECK(ratio < (restart ? 20 : 12), "%u %s: %s clicks %+.1f dB", p->number, p->name, what, ratio);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        only = atoi(argv[1]);
    }
    if (only < 0) {
        check_laws();
    }
    unsigned per_algo[R80_ALGOS] = { 0 };
    for (unsigned i = 0; i < r80_program_count; i++) {
        const struct r80_program *p = &r80_programs[i];
        if (only >= 0 && p->number != only) {
            continue;
        }
        check_literal(p);
        check_decay(p);
        check_extremes(p);
        if (per_algo[p->algo]++ < 2 || only >= 0) {
            int lo, hi;
            r80_edit_range(p, R80_E_MIDRT, &lo, &hi);
            if (p->algo != R80_INVERSE) {
                check_edit(p, R80_E_SIZE, p->algo == R80_HALL ? 200 : 60, "Size change");
                check_edit(p, R80_E_MIDRT, 30, "Mid Rt change");
            } else {
                check_edit(p, R80_E_DURATION, 80, "Duration change");
            }
            check_edit(p, R80_E_HIGHCUT, 60, "High Cut change");
        }
    }
    printf("worst literal match %.1f dB\n", snr_worst);
    printf(fails ? "r80test: %d FAILED\n" : "r80test: ok\n", fails);
    r80_load(0);
    return fails != 0;
}
