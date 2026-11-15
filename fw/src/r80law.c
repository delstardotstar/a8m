#include "r80.h"

#include <string.h>

/* The parameter laws of the spec (section 4, 3, 3.1, 4.10, 16.6):
 * values -> tank coefficients and offsets and the input/output values.
 * tools/r80test.c holds every program's result to the program data. */

extern const uint16_t r80_damp_hz[60], r80_adjust_t[81], r80_curves[5 * 257], r80_plate_a0[127], r80_chamber_a0[127];
extern const uint16_t r80_hall_a0[127], r80_hall_a1[127], r80_inv_a0[127], r80_inv_a1[127], r80_inv_listed[7 * 113];
extern const uint8_t r80_depth_16[48], r80_hall_half[127], r80_inv_j[33], r80_divider[11];
extern const uint32_t r80_hicut[122];

#define FS      48000
#define PI      3.14159265358979323846
#define LN2     0.69314718055994530942
#define LN10    2.30258509299404568402

/* Double math for the laws, alike on the host and the module (no libm
 * there): series after range reduction, within a few ulp. */
static double m_floor(double x)
{
    double t = (double)(int64_t)x;

    return t > x ? t - 1.0 : t;
}

static double ldexp2(double x, int k)
{
    for (; k > 0; k--) {
        x *= 2.0;
    }
    for (; k < 0; k++) {
        x *= 0.5;
    }
    return x;
}

static double m_exp(double x)
{
    int k = (int)m_floor(x / LN2 + 0.5);
    double r = x - k * LN2, t = 1.0, s = 1.0;

    for (int i = 1; i < 18; i++) {
        t *= r / i;
        s += t;
    }
    return ldexp2(s, k);
}

static double m_log(double x)
{
    int e = 0;

    while (x > 1.41421356237309504880) {
        x *= 0.5;
        e++;
    }
    while (x < 0.70710678118654752440) {
        x *= 2.0;
        e--;
    }
    double z = (x - 1.0) / (x + 1.0), z2 = z * z, t = z, s = 0.0;
    for (int i = 1; i < 40; i += 2) {
        s += t / i;
        t *= z2;
    }
    return 2.0 * s + e * LN2;
}

static double m_pow(double b, double y)
{
    return b == 10.0 ? m_exp(y * LN10) : m_exp(y * m_log(b));
}

static double m_sin(double x)
{
    while (x > PI) {
        x -= 2.0 * PI;
    }
    while (x < -PI) {
        x += 2.0 * PI;
    }
    double t = x, s = x, x2 = x * x;
    for (int i = 2; i < 40; i += 2) {
        t *= -x2 / (i * (i + 1));
        s += t;
    }
    return s;
}

static double m_cos(double x) { return m_sin(x + PI / 2.0); }

const struct r80_program *r80_find(unsigned number)
{
    for (unsigned i = 0; i < r80_program_count; i++) {
        if (r80_programs[i].number == number) {
            return &r80_programs[i];
        }
    }
    return 0;
}

/* Parameters whose patches keep 1/32 of a unit (4.10). */
static int fine(unsigned p)
{
    return (p >= R80_P_DLY_V1 && p <= R80_P_DLY_V6) || p == R80_P_PSTDLY_L || p == R80_P_PSTDLY_R ||
           p == R80_P_RGLDRANGE || p == R80_P_VGLDRANGE || (p >= R80_P_DEPTH_V1 && p <= R80_P_DEPTH_V6);
}

static const uint16_t pmax[R80_PARAMS] = {
    [R80_P_INLVL_R] = 160, [R80_P_INLVL_L] = 160, [R80_P_INPAN_R] = 100, [R80_P_INPAN_L] = 100,
    [R80_P_FXMIX] = 100, [R80_P_FXADJUST] = 80, [R80_P_FXWIDTH] = 720, [R80_P_VOICEDIF] = 100,
    [R80_P_HIGHCUT] = 121, [R80_P_RTHC] = 48, [R80_P_LOWRT] = 9, [R80_P_XOVER] = 60, [R80_P_MIDRT] = 63,
    [R80_P_PREDELAY] = 465, [R80_P_EKODLY_L] = 600, [R80_P_EKODLY_R] = 600, [R80_P_EKOFBK_L] = 30,
    [R80_P_EKOFBK_R] = 30, [R80_P_REFDLY_L] = 600, [R80_P_REFDLY_R] = 600, [R80_P_REFLVL_L] = 15,
    [R80_P_REFLVL_R] = 15, [R80_P_PSTDLY_L] = 1365, [R80_P_PSTDLY_R] = 1365, [R80_P_PSTMIX] = 100,
    [R80_P_RGLDRESP] = 100, [R80_P_RGLDRANGE] = 1365, [R80_P_LOWSLOPE] = 32, [R80_P_MIDSLOPE] = 32,
    [R80_P_SIZE] = 144, [R80_P_APSWEEP] = 50, [R80_P_DIFFUSION] = 100, [R80_P_SPREAD] = 255, [R80_P_SHAPE] = 255,
    [R80_P_ATTACK] = 100, [R80_P_LINK] = 1, [R80_P_RVBOUT] = 15, [R80_P_RVBIN] = 80, [R80_P_RVBWIDTH] = 720,
    [R80_P_DEF] = 100, [R80_P_DEPTH] = 15, [R80_P_CHORUS] = 10, [R80_P_DURATION] = 112,
    [R80_P_LVL_MASTER] = 80, [R80_P_LVL_V1] = 160, [R80_P_LVL_V2] = 160, [R80_P_LVL_V3] = 160,
    [R80_P_LVL_V4] = 160, [R80_P_LVL_V5] = 160, [R80_P_LVL_V6] = 160, [R80_P_DLY_MASTER] = 200,
    [R80_P_DLY_V1] = 1365, [R80_P_DLY_V2] = 1365, [R80_P_DLY_V3] = 1365, [R80_P_DLY_V4] = 1365,
    [R80_P_DLY_V5] = 1365, [R80_P_DLY_V6] = 1365, [R80_P_CLEAR] = 1, [R80_P_VGLDRESP] = 100,
    [R80_P_VGLDRANGE] = 1365, [R80_P_MSTDEPTH] = 200, [R80_P_DEPTH_V1] = 500, [R80_P_DEPTH_V2] = 500,
    [R80_P_DEPTH_V3] = 500, [R80_P_DEPTH_V4] = 500, [R80_P_DEPTH_V5] = 500, [R80_P_DEPTH_V6] = 500,
    [R80_P_MSTRATE] = 200, [R80_P_RATE_V1] = 100, [R80_P_RATE_V2] = 100, [R80_P_RATE_V3] = 100,
    [R80_P_RATE_V4] = 100, [R80_P_RATE_V5] = 100, [R80_P_RATE_V6] = 100, [R80_P_FB_MASTER] = 100,
    [R80_P_FB_V1] = 200, [R80_P_FB_V2] = 200, [R80_P_FB_V3] = 200, [R80_P_FB_V4] = 200, [R80_P_FB_V5] = 200,
    [R80_P_FB_V6] = 200, [R80_P_PAN_MASTER] = 100, [R80_P_PAN_V1] = 100, [R80_P_PAN_V2] = 100,
    [R80_P_PAN_V3] = 100, [R80_P_PAN_V4] = 100, [R80_P_PAN_V5] = 100, [R80_P_PAN_V6] = 100,
    [R80_P_LFO_RATE] = 2500, [R80_P_LFO_SHAPE] = 5, [R80_P_LFO_PWIDTH] = 98, [R80_P_LFO_DEPTH] = 100,
    [R80_P_ENVL_REL] = 500, [R80_P_ENVR_REL] = 500, [R80_P_AR_ATTACK] = 500, [R80_P_AR_RELEASE] = 500,
    [R80_P_AR_MODE] = 3, [R80_P_SW1_MODE] = 2, [R80_P_SW1_RATE] = 2500, [R80_P_SW1_PWIDTH] = 98,
    [R80_P_SW2_MODE] = 2, [R80_P_SW2_RATE] = 2500, [R80_P_SW2_PWIDTH] = 98,
};

static int param_max(const struct r80_program *p, unsigned q)
{
    if (q == R80_P_SIZE && p->algo == R80_HALL) {
        return 304;
    }
    if ((q == R80_P_REFDLY_L || q == R80_P_REFDLY_R) && p->algo == R80_INVERSE) {
        return 400;
    }
    if (q == R80_P_SPREAD && (p->algo == R80_PLATE || p->algo == R80_CHORUS)) {
        return 100;
    }
    if (q == R80_P_ATTACK && p->algo == R80_HALL) {
        return 255;
    }
    return pmax[q];
}

void r80_stored(const struct r80_program *p, struct r80_values *v)
{
    for (unsigned q = 0; q < R80_PARAMS; q++) {
        unsigned x = p->value[q];

        if ((p->locked[q >> 5] >> (q & 31)) & 1u) {
            /* 32 den + num -> the word 256 num + den. */
            v->v[q] = (int32_t)(((x & 31u) << 8) | (x >> 5));
        } else {
            v->v[q] = (int32_t)(x << 5);
        }
    }
    memcpy(v->locked, p->locked, sizeof(v->locked));
}

static int32_t floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return (int32_t)(q * b > a ? q - 1 : q);
}

static int32_t ceildiv(int64_t a, int64_t b)
{
    return -floordiv(-a, b);
}

/* num or den of a beat point: 0 or above 24 makes the point 1/1. */
static void beat_point(unsigned y, int *num, int *den)
{
    *num = (int)(y >> 5);
    *den = (int)(y & 31u);
    if (!*num || !*den || *num > 24 || *den > 24) {
        *num = *den = 1;
    }
}

static int beat_interp(int a, int b, int dx, int x)
{
    int step = (256 * (b - a)) / dx;   /* toward zero */

    return (256 * a + x * step) >> 8;
}

int32_t r80_patch_value(const struct r80_patch *q, unsigned x, int locked)
{
    unsigned n = q->points, i;

    if (locked) {
        /* 16.6: num and den apart, in 8.8 steps. */
        int n0, d0, n1, d1;
        if (!q->beats) {
            return 256 + 1;
        }
        if (x <= q->x[0]) {
            beat_point(q->y[0], &n0, &d0);
            return 256 * n0 + d0;
        }
        for (i = 0; i + 1 < n && x >= q->x[i + 1]; i++) {
        }
        beat_point(q->y[i], &n0, &d0);
        if (i + 1 >= n) {
            return 256 * n0 + d0;
        }
        beat_point(q->y[i + 1], &n1, &d1);
        int dx = q->x[i + 1] - q->x[i], t = (int)x - q->x[i];
        return 256 * beat_interp(n0, n1, dx, t) + beat_interp(d0, d1, dx, t);
    }
    if (q->beats) {
        return 0;
    }
    /* 4.10 in the destination's scale: the parameter's or 1/32 of it. */
    int max = pmax[q->dest], m = fine(q->dest) ? 32 * max : max;
    int32_t t[R80_POINTS_MAX] = { 0 };
    if (q->dest == R80_P_SIZE || q->dest == R80_P_REFDLY_L || q->dest == R80_P_REFDLY_R) {
        /* Ranges that depend on the algorithm: their own units (M = max). */
        m = max = 65535;
    }
    for (i = 0; i < n; i++) {
        int64_t u = ((int64_t)65535 * q->y[i] + max / 2) / max;
        t[i] = (int32_t)((u * m + 32767) / 65535);
    }
    int32_t y;
    if (x <= q->x[0]) {
        y = t[0];
    } else {
        for (i = 0; i + 1 < n && x >= q->x[i + 1]; i++) {
        }
        if (i + 1 >= n) {
            y = t[n - 1];
        } else {
            int dx = q->x[i + 1] - q->x[i], d = (int)x - q->x[i];
            if (t[i + 1] >= t[i]) {
                int32_t s = floordiv((int64_t)(t[i + 1] - t[i]) * 65536, dx);
                y = t[i] + floordiv((int64_t)s * d, 65536);
            } else {
                int32_t s = floordiv((int64_t)(t[i] - t[i + 1]) * 65536, dx);
                y = t[i] - ceildiv((int64_t)s * d, 65536);
            }
        }
    }
    return m == max ? y * 32 : y;
}

void r80_patch_all(const struct r80_program *p, const struct r80_values *s, const int16_t *src, struct r80_values *v)
{
    int32_t sum[R80_PARAMS];
    uint8_t any[R80_PARAMS];

    *v = *s;
    memset(any, 0, sizeof(any));
    for (unsigned i = 0; i < p->npatches; i++) {
        const struct r80_patch *q = &p->patch[i];
        int x = src[q->source];

        if (x < 0) {
            continue;
        }
        if (!any[q->dest]) {
            any[q->dest] = 1;
            sum[q->dest] = 0;
        }
        sum[q->dest] += r80_patch_value(q, (unsigned)x, r80_locked(s, q->dest));
    }
    for (unsigned d = 0; d < R80_PARAMS; d++) {
        if (!any[d]) {
            continue;
        }
        if (r80_locked(s, d)) {
            v->v[d] = sum[d] & 0xffff;
        } else {
            int32_t hi = 32 * param_max(p, d);
            v->v[d] = sum[d] < 0 ? 0 : sum[d] > hi ? hi : sum[d];
        }
    }
}

/* ---- tempo (4.11, 16.3) ---- */

static int32_t beat_samples(const struct r80_program *p, int32_t word)
{
    int num = word >> 8, den = word & 255;
    int32_t d = 960000 / p->tempo, r = 4096000 / d;

    if (!num || !den) {
        num = den = 1;
    }
    return (int32_t)((int64_t)12288000 * num / ((int64_t)r * den));
}

/* A plain value in whole units (patches can leave 1/32 steps). */
static int unit(const struct r80_values *v, unsigned q)
{
    return v->v[q] >> 5;
}

/* Tank offsets of Pre Delay, RefDly, EkoDly: 1 + 96 v, or S + 1 halved
 * into the range (4.11). */
static int32_t tank_time(const struct r80_program *p, const struct r80_values *v, unsigned q)
{
    if (r80_locked(v, q)) {
        int32_t s = beat_samples(p, v->v[q]), lim = 96 * param_max(p, q);
        while (s > lim) {
            s >>= 1;
        }
        return s + 1;
    }
    return 1 + 96 * unit(v, q);
}

/* ---- tank coefficients ---- */

static int lvl16(int v) { return 16 * v; }

/* One-pole b (x 256) of table entry e (4.4); 60 is Full. */
static int damp_b(int e)
{
    return e >= 60 ? 0 : (int)m_floor(256.0 * m_exp(-2.0 * PI * r80_damp_hz[e] / FS) + 0.5);
}

static int clamp255(int k) { return k > 255 ? 255 : k < -255 ? -255 : k; }

static int curve(int c, int v)
{
    return r80_curves[257 * c + (v >= 254 ? 256 : v)];
}

static int shape_k(int c, int v) { return 16 * (curve(c, v) / 2048); }

/* Chamber "a" coefficients: scaled by the damping a (4.7). */
static int shape_ka(int c, int v, int a256)
{
    return 16 * ceildiv((int64_t)curve(c, v) * a256, 1 << 19);
}

struct decay_tank { uint16_t l, m, u; };

static void decay(const struct r80_program *p, const struct r80_values *v, int e_scale, int *g256, int *low)
{
    static const struct decay_tank dt[3] = { { 5252, 10, 6 }, { 5253, 10, 6 }, { 2186, 20, 2 } };
    const struct decay_tank *d = &dt[p->algo == R80_CHAMBER ? 0 : p->algo == R80_HALL ? 2 : 1];
    int b = d->l / 48, size = unit(v, R80_P_SIZE);
    int kraw = unit(v, R80_P_LINK) ? b * d->m : b * (16 + size) / 16, k = kraw / d->u;
    double g = m_pow(10.0, -3.0 * k / r80_rt_ms[unit(v, R80_P_MIDRT)]);
    static const double m[10] = { 0.2, 0.4, 0.6, 0.8, 1.0, 1.2, 1.5, 2.0, 3.0, 4.0 };
    double e = e_scale * (1.0 / m[unit(v, R80_P_LOWRT)] - 1.0);

    *g256 = (int)m_floor(256.0 * g);
    *low = clamp255((int)(256.0 * g * (m_pow(g, e) - 1.0)));
}

/* Diffusion gains (4.5) x 256: g1, g2, and their x2-coded magnitudes. */
static void diffusion(int v, int *g1, int *g2)
{
    *g1 = 192 * v / 100;
    *g2 = 168 * v / 100;
}

static int x2(int g) { return 2 * (g / 2); }

static int eko(int v)
{
    int d = v - 15, m = (int)m_floor(127.0 * (d < 0 ? -d : d) / 15.0 + 0.5);

    return d > 0 ? 2 * m : d < 0 ? -2 * m : 0;
}

/* AP Sweep pairs: (first, second) coefficient, sign of the first (6.1). */
static const uint8_t ap_plate[6][2] = { { 123, 125 }, { 3, 5 }, { 11, 13 }, { 29, 31 }, { 36, 38 }, { 44, 46 } };
static const uint8_t ap_chamber[6][2] = { { 0, 2 }, { 6, 8 }, { 15, 17 }, { 34, 36 }, { 40, 42 }, { 49, 51 } };
static const int8_t ap_chamber_sign[6] = { 1, -1, 1, -1, 1, -1 };
static const uint8_t ap_hall[10][2] = { { 46, 52 }, { 48, 50 }, { 54, 64 }, { 56, 62 }, { 58, 60 },
                                        { 91, 97 }, { 93, 95 }, { 99, 109 }, { 101, 107 }, { 103, 105 } };
static const uint8_t ap_hall_large[10] = { 1, 0, 1, 0, 0, 1, 0, 1, 0, 0 };
/* Hall pairs whose first coefficient is coded as a half, doubled (6.1). */
static const uint8_t ap_hall_halved[10] = { 0, 1, 0, 0, 1, 0, 1, 0, 0, 1 };

/* The first coefficient's magnitude of such a pair, from the second's
 * gain g (x 256): its code c = 255 - g halved, doubled back. */
int r80_ap_first(int g)
{
    int c = 255 - g, ca = (c - !((c >> 1) & 1)) >> 1;

    return 2 * (127 - ca);
}

/* Every AP Sweep pair at its rest gain with the stock signs. */
static void ap_rest(const struct r80_program *p, const struct r80_values *v, int16_t *k)
{
    if (p->algo == R80_HALL) {
        int def = unit(v, R80_P_DEF);
        for (int j = 0; j < 10; j++) {
            int g = (ap_hall_large[j] ? 128 : 64) * (100 - def) / 100;
            k[ap_hall[j][0]] = (int16_t)(ap_hall_halved[j] ? r80_ap_first(g) : g);
            k[ap_hall[j][1]] = (int16_t)-g;
        }
    } else if (p->algo == R80_CHAMBER) {
        for (int j = 0; j < 6; j++) {
            k[ap_chamber[j][0]] = (int16_t)(144 * ap_chamber_sign[j]);
            k[ap_chamber[j][1]] = (int16_t)(-144 * ap_chamber_sign[j]);
        }
    } else if (p->algo != R80_INVERSE) {
        for (int j = 0; j < 6; j++) {
            k[ap_plate[j][0]] = -128;
            k[ap_plate[j][1]] = 128;
        }
    }
}

static void tank_k(const struct r80_program *p, const struct r80_values *v, int16_t *k)
{
    int rthc = unit(v, R80_P_RTHC), xo = unit(v, R80_P_XOVER);
    int db = damp_b(rthc + 12), da = 256 - db, xb = damp_b(xo), xa = 256 - xb;
    int g1, g2, g, low;

    diffusion(unit(v, R80_P_DIFFUSION), &g1, &g2);
    memset(k, 0, sizeof(int16_t) * R80_K);
    ap_rest(p, v, k);
    switch (p->algo) {
    case R80_CHAMBER: {
        decay(p, v, 3, &g, &low);
        k[4] = k[11] = k[30] = k[38] = k[120] = (int16_t)g;
        k[45] = (int16_t)-g;
        k[32] = k[122] = (int16_t)low;
        k[20] = k[109] = (int16_t)da;
        k[22] = k[111] = (int16_t)db;
        k[26] = k[116] = (int16_t)xb;
        k[28] = k[118] = (int16_t)xa;
        int s = unit(v, R80_P_SHAPE);
        k[13] = k[47] = (int16_t)shape_k(0, s);
        k[24] = k[113] = (int16_t)shape_ka(1, s, da);
        k[14] = k[48] = (int16_t)shape_k(2, s);
        k[25] = k[114] = (int16_t)shape_ka(3, s, da);
        k[68] = k[86] = (int16_t)shape_k(4, s);
        k[69] = (int16_t)lvl16(unit(v, R80_P_REFLVL_R));
        k[87] = (int16_t)lvl16(unit(v, R80_P_REFLVL_L));
        k[70] = k[88] = (int16_t)lvl16(unit(v, R80_P_RVBOUT));
        k[89] = k[99] = (int16_t)-g1;
        k[93] = k[103] = (int16_t)g1;
        k[95] = k[105] = (int16_t)-x2(g2);
        k[97] = k[107] = (int16_t)g2;
        k[91] = (int16_t)eko(unit(v, R80_P_EKOFBK_R));
        k[101] = (int16_t)eko(unit(v, R80_P_EKOFBK_L));
        break;
    }
    case R80_PLATE:
    case R80_CHORUS: {
        decay(p, v, 3, &g, &low);
        k[0] = k[8] = k[24] = k[33] = k[118] = (int16_t)g;
        k[41] = (int16_t)-g;
        k[26] = k[120] = (int16_t)low;
        k[16] = k[114] = (int16_t)da;
        k[18] = k[116] = (int16_t)db;
        k[20] = k[81] = (int16_t)xb;
        k[22] = k[83] = (int16_t)xa;
        int s = unit(v, R80_P_ATTACK);
        k[10] = k[43] = (int16_t)shape_k(0, s);
        k[28] = k[122] = (int16_t)shape_k(1, s);
        k[2] = k[35] = (int16_t)shape_k(3, s);
        k[61] = k[78] = (int16_t)shape_k(4, s);
        k[62] = (int16_t)lvl16(unit(v, R80_P_REFLVL_R));
        k[79] = (int16_t)lvl16(unit(v, R80_P_REFLVL_L));
        k[63] = k[80] = (int16_t)lvl16(unit(v, R80_P_RVBOUT));
        k[86] = k[100] = (int16_t)-g1;
        k[90] = k[104] = (int16_t)g1;
        k[92] = k[96] = k[106] = k[110] = (int16_t)-x2(g2);
        k[94] = k[98] = k[108] = k[112] = (int16_t)g2;
        k[88] = (int16_t)eko(unit(v, R80_P_EKOFBK_R));
        k[102] = (int16_t)eko(unit(v, R80_P_EKOFBK_L));
        break;
    }
    case R80_HALL: {
        decay(p, v, 1, &g, &low);
        k[27] = k[71] = (int16_t)g;
        k[29] = k[73] = (int16_t)low;
        k[34] = k[78] = (int16_t)db;
        k[36] = (int16_t)da;
        k[80] = (int16_t)-da;
        k[11] = k[68] = (int16_t)xb;
        k[9] = k[66] = (int16_t)xa;
        int d = unit(v, R80_P_DEPTH);
        k[13] = k[20] = (int16_t)(16 * r80_depth_16[d]);
        k[15] = k[18] = (int16_t)(16 * r80_depth_16[16 + d]);
        k[16] = k[21] = (int16_t)(16 * r80_depth_16[32 + d]);
        k[14] = k[19] = (int16_t)(-16 * r80_depth_16[32 + d]);
        k[23] = k[25] = (int16_t)lvl16(unit(v, R80_P_RVBOUT));
        k[24] = (int16_t)lvl16(unit(v, R80_P_REFLVL_R));
        k[26] = (int16_t)lvl16(unit(v, R80_P_REFLVL_L));
        k[31] = k[75] = 176;
        k[32] = k[76] = 32;
        k[33] = k[77] = 16;
        k[38] = k[83] = (int16_t)-x2(g1);
        k[40] = k[85] = (int16_t)g1;
        k[42] = k[87] = (int16_t)-x2(g2);
        k[44] = k[89] = (int16_t)g2;
        break;
    }
    case R80_INVERSE: {
        int mid = unit(v, R80_P_MIDSLOPE), lo = unit(v, R80_P_LOWSLOPE);
        k[4] = k[30] = k[38] = k[121] = (int16_t)clamp255(16 * (mid - 16));
        k[32] = k[123] = (int16_t)clamp255(16 * (lo - mid));
        k[17] = k[51] = r80_inv_j[mid];
        k[22] = k[112] = (int16_t)da;
        k[24] = k[114] = (int16_t)db;
        k[26] = k[117] = (int16_t)xb;
        k[28] = k[119] = (int16_t)xa;
        int s = unit(v, R80_P_SHAPE);
        k[11] = k[45] = (int16_t)shape_k(0, s);
        k[12] = k[46] = (int16_t)shape_k(1, s);
        k[13] = k[47] = (int16_t)shape_k(2, s);
        k[14] = k[48] = (int16_t)shape_k(3, s);
        k[70] = k[87] = (int16_t)shape_k(4, s);
        k[69] = (int16_t)lvl16(unit(v, R80_P_REFLVL_R));
        k[86] = (int16_t)lvl16(unit(v, R80_P_REFLVL_L));
        for (int s2 = 88; s2 <= 108; s2 += 4) {
            k[s2] = (int16_t)-x2(g1);
            k[s2 + 2] = (int16_t)g1;
        }
        break;
    }
    }
}

/* ---- tank offsets ---- */

static void tank_a(const struct r80_program *p, const struct r80_values *v, uint16_t *a)
{
    int32_t pd = tank_time(p, v, R80_P_PREDELAY);
    int size = unit(v, R80_P_SIZE), link = unit(v, R80_P_LINK);

    memset(a, 0, sizeof(uint16_t) * R80_K);
    switch (p->algo) {
    case R80_PLATE:
    case R80_CHORUS: {
        int s = link ? size : 144, q = (16 + s) / 2;
        for (int i = 0; i < R80_K; i++) {
            if (r80_plate_a0[i]) {
                a[i] = (uint16_t)(r80_plate_a0[i] + (r80_plate_a0[i] - 20) * size / 16);
            }
        }
        a[2] = a[35] = (uint16_t)(pd + 16444 * q / 100);
        a[10] = a[43] = (uint16_t)(pd + 878 * q / 100);
        a[28] = a[122] = (uint16_t)(pd + 2390 * q / 100);
        a[61] = a[78] = (uint16_t)pd;
        a[62] = (uint16_t)tank_time(p, v, R80_P_REFDLY_R);
        a[79] = (uint16_t)tank_time(p, v, R80_P_REFDLY_L);
        a[88] = (uint16_t)tank_time(p, v, R80_P_EKODLY_R);
        a[102] = (uint16_t)tank_time(p, v, R80_P_EKODLY_L);
        break;
    }
    case R80_CHAMBER: {
        static const uint8_t at[7] = { 13, 14, 24, 25, 48, 113, 114 };
        static const uint16_t m[7] = { 878, 13488, 4497, 20736, 8154, 2390, 16444 };
        int spread = unit(v, R80_P_SPREAD), q = link ? spread * (16 + size) / 160 : spread;
        for (int i = 0; i < R80_K; i++) {
            if (r80_chamber_a0[i]) {
                a[i] = (uint16_t)(r80_chamber_a0[i] + (r80_chamber_a0[i] - 33) * size / 16);
            }
        }
        for (int j = 0; j < 7; j++) {
            a[at[j]] = (uint16_t)(pd + m[j] * q / 255);
        }
        a[47] = a[68] = a[86] = (uint16_t)pd;
        a[69] = (uint16_t)tank_time(p, v, R80_P_REFDLY_R);
        a[87] = (uint16_t)tank_time(p, v, R80_P_REFDLY_L);
        a[91] = (uint16_t)tank_time(p, v, R80_P_EKODLY_R);
        a[101] = (uint16_t)tank_time(p, v, R80_P_EKODLY_L);
        break;
    }
    case R80_HALL: {
        static const uint8_t in[8] = { 17, 22, 31, 32, 33, 75, 76, 77 };
        for (int i = 0; i < R80_K; i++) {
            int a0 = r80_hall_a0[i], a1 = r80_hall_a1[i];
            if (a0) {
                a[i] = (uint16_t)(r80_hall_half[i] ? a0 + (2 * (a1 - a0) + 1) * size / 608 : a0 + (a1 - a0) * size / 304);
            }
        }
        for (int j = 0; j < 8; j++) {
            a[in[j]] = (uint16_t)(26626 + pd);
        }
        a[24] = (uint16_t)(26626 + tank_time(p, v, R80_P_REFDLY_R));
        a[26] = (uint16_t)(26626 + tank_time(p, v, R80_P_REFDLY_L));
        /* The modulated taps (6.2): the second read in its window, the first past it. */
        static const uint8_t ta[4] = { 109, 117, 123, 3 }, tb[4] = { 112, 118, 124, 4 };
        for (int j = 0; j < 4; j++) {
            unsigned base = 256u + 512u * (unsigned)j, b = p->tap[j] | 1u;
            if (b == 255u) {
                b = 253u;
            }
            a[tb[j]] = (uint16_t)(base + b);
            a[ta[j]] = (uint16_t)(base + b + 1u);
        }
        break;
    }
    case R80_INVERSE: {
        static const uint8_t listed[7] = { 7, 8, 13, 14, 42, 46, 47 };
        int dur = unit(v, R80_P_DURATION);
        int32_t shift = pd - 289;
        for (int i = 0; i < R80_K; i++) {
            int a0 = r80_inv_a0[i], a1 = r80_inv_a1[i];
            if (a0) {
                a[i] = (uint16_t)(a0 + (a1 - a0) * dur / 112);
            }
        }
        for (int j = 0; j < 7; j++) {
            a[listed[j]] = (uint16_t)(r80_inv_listed[113 * j + dur] + shift);
        }
        a[39] = a[87] = (uint16_t)(32768 + pd);
        a[70] = (uint16_t)pd;
        int32_t base = 2701 + 18914 * dur / 112 + pd;
        a[69] = (uint16_t)(base + tank_time(p, v, R80_P_REFDLY_R));
        a[86] = (uint16_t)(base + tank_time(p, v, R80_P_REFDLY_L) + 32768);
        break;
    }
    }
}

/* ---- input/output section (3, 3.1) ---- */

/* The input-level law (4.5). */
static double inlvl(int v)
{
    return v > 80 ? m_pow(10.0, (v - 160) / 20.0) : v == 80 ? 0.0 : -m_pow(10.0, -v / 20.0);
}

static double eqp(int v) { return m_sin(PI * v / 200.0); }

static void width(int v, double *same, double *cross)
{
    double th = (45.0 + v - 0.1 * (v % 9)) * PI / 180.0;

    *same = m_sin(th);
    *cross = m_cos(th);
}

static double out_gain(int v)
{
    unsigned t = r80_adjust_t[v];

    return 4.0 * (256.0 * t + t % 256) / 8388608.0;
}

/* Voice delay in 1/32 ms (4.12): n = 32 t M / 100. */
static int32_t voice_n(const struct r80_program *p, const struct r80_values *v, unsigned q)
{
    int m = unit(v, R80_P_DLY_MASTER);

    if (r80_locked(v, q)) {
        int32_t s = beat_samples(p, v->v[q]);
        while (s > 1365 * 48) {
            s >>= 1;
        }
        return (int32_t)(32 * (s / 48) * m / 100);
    }
    return (int32_t)((int64_t)v->v[q] * m / 100);
}

static void voice_gains(const struct r80_values *v, unsigned j, double *l, double *r, double *fb)
{
    int lv = unit(v, R80_P_LVL_V1 + j), ml = unit(v, R80_P_LVL_MASTER) - 40;
    int pn = unit(v, R80_P_PAN_V1 + j) + unit(v, R80_P_PAN_MASTER) - 50;
    double g;

    /* The master adds dB: an inverted voice (below 80) moves down. */
    if (lv > 80) {
        lv = lv + ml < 80 ? 80 : lv + ml > 160 ? 160 : lv + ml;
    } else if (lv < 80) {
        lv = lv - ml > 80 ? 80 : lv - ml < 0 ? 0 : lv - ml;
    }
    pn = pn < 0 ? 0 : pn > 100 ? 100 : pn;
    g = inlvl(lv) * 32767.0 / 32768.0;
    *l = g * (1.0 - pn / 100.0);
    *r = g * pn / 100.0;
    *fb = (unit(v, R80_P_FB_V1 + j) - 100) / 100.0 * unit(v, R80_P_FB_MASTER) / 100.0;
}

static void io3(const struct r80_program *p, const struct r80_values *v, double *io)
{
    double ll = inlvl(unit(v, R80_P_INLVL_L)), lr = inlvl(unit(v, R80_P_INLVL_R));
    int pl = unit(v, R80_P_INPAN_L), pr = unit(v, R80_P_INPAN_R);

    io[R80_IO_ALL] = ll * (1.0 - pl / 100.0);
    io[R80_IO_ARL] = ll * pl / 100.0;
    io[R80_IO_ALR] = lr * (1.0 - pr / 100.0);
    io[R80_IO_ARR] = lr * pr / 100.0;
    io[R80_IO_GS] = 0.4375 * m_pow(10.0, (unit(v, R80_P_RVBIN) - 80) / 20.0);
    io[R80_IO_GR] = 1.0;
    for (int s = 0; s < 2; s++) {
        unsigned q = s ? R80_P_PSTDLY_R : R80_P_PSTDLY_L;
        io[R80_IO_POSTL + s] = r80_locked(v, q) ? (double)beat_samples(p, v->v[q]) : 1.5 * v->v[q];
    }
    io[R80_IO_PD] = eqp(unit(v, R80_P_PSTMIX));
    io[R80_IO_PR] = eqp(100 - unit(v, R80_P_PSTMIX));
    width(unit(v, R80_P_RVBWIDTH), &io[R80_IO_C1], &io[R80_IO_C2]);
    int vd = unit(v, R80_P_VOICEDIF);
    io[R80_IO_G] = vd / 200.0;
    io[R80_IO_VD] = vd > 0;
    io[R80_IO_VP] = vd == 0;
    for (unsigned j = 0; j < 4; j++) {
        io[R80_IO_DV1 + j] = 1.5 * voice_n(p, v, R80_P_DLY_V1 + j);
        voice_gains(v, j, &io[R80_IO_V1L + 3 * j], &io[R80_IO_V1L + 3 * j + 1], &io[R80_IO_V1L + 3 * j + 2]);
    }
    io[R80_IO_FV] = eqp(100 - unit(v, R80_P_FXMIX));
    io[R80_IO_FR] = eqp(unit(v, R80_P_FXMIX));
    io[R80_IO_A] = r80_hicut[unit(v, R80_P_HIGHCUT)] / 8388608.0;
    width(unit(v, R80_P_FXWIDTH), &io[R80_IO_W1], &io[R80_IO_W2]);
    io[R80_IO_OUT] = out_gain(unit(v, R80_P_FXADJUST));
}

/* LFO frequency in 0.01 Hz (4.12). */
static int chorus_hz(int r)
{
    return r <= 20 ? r : r <= 50 ? 20 + 2 * (r - 20) : r <= 55 ? 80 + 4 * (r - 50) : r <= 95 ? 100 + 5 * (r - 55) :
           300 + 10 * (r - 95);
}

static void io31(const struct r80_program *p, const struct r80_values *v, double *io)
{
    double ll = inlvl(unit(v, R80_P_INLVL_L)), lr = inlvl(unit(v, R80_P_INLVL_R));
    int pl = unit(v, R80_P_INPAN_L), pr = unit(v, R80_P_INPAN_R);
    int md = unit(v, R80_P_MSTDEPTH), mr = unit(v, R80_P_MSTRATE);
    extern const uint16_t r80_glide[101];

    io[R80_C_90] = lr * pr / 100.0;
    io[R80_C_91] = lr * (1.0 - pr / 100.0);
    io[R80_C_92] = ll * pl / 100.0;
    io[R80_C_93] = ll * (1.0 - pl / 100.0);
    io[R80_C_94] = 0.4375;
    for (unsigned j = 0; j < 6; j++) {
        double l, r, fb;
        voice_gains(v, j, &l, &r, &fb);
        io[R80_C_FB1 + j] = fb;
        io[R80_C_CL1 + j] = l;
        io[R80_C_CR1 + j] = r;
        io[R80_C_B1 + j] = 128.0 + 192.0 * voice_n(p, v, R80_P_DLY_V1 + j);
        int32_t d16 = (int32_t)((int64_t)v->v[R80_P_DEPTH_V1 + j] * md / 100);
        io[R80_C_A1 + j] = 128 + 96 * d16 / 5;
        int32_t h = chorus_hz(unit(v, R80_P_RATE_V1 + j)) * mr / 100;
        io[R80_C_R1 + j] = (double)(int32_t)(((int64_t)h << 24) / 4800000);
    }
    io[R80_C_DAMP] = r80_hicut[unit(v, R80_P_HIGHCUT)] / 8388608.0;
    io[R80_C_GLIDE] = r80_glide[unit(v, R80_P_VGLDRESP)] / 8388608.0;
    io[R80_C_38] = 1.0 / 8192.0;
    io[R80_C_U] = 238627;
    io[R80_C_U0] = 1193047;
    io[R80_C_N] = 4800;
    io[R80_C_AC] = eqp(100 - unit(v, R80_P_FXMIX));
    io[R80_C_AD] = eqp(unit(v, R80_P_FXMIX));
    width(unit(v, R80_P_FXWIDTH), &io[R80_C_AE], &io[R80_C_AF]);
    io[R80_C_OUT] = out_gain(unit(v, R80_P_FXADJUST));
}

/* LFO and switch rates: phase steps per control step, free or one cycle
 * per num/den beats (6.3, 16.3). */
static uint32_t rate_inc(const struct r80_program *p, const struct r80_values *v, unsigned q)
{
    if (r80_locked(v, q)) {
        int num = v->v[q] >> 8, den = v->v[q] & 255;
        int32_t d = 960000 / p->tempo, r = 4096000 / d;
        if (!num || !den) {
            num = den = 1;
        }
        return (uint32_t)((131072 * den / (50 * num)) * r / 256);
    }
    return (6710u * (uint32_t)unit(v, q) + 128u) / 256u;
}

static uint16_t steps(int n)
{
    n = n > 500 ? 500 : n;
    return (uint16_t)(n ? 65535 / n : 65535);
}

static uint16_t pulse_width(int v) { return (uint16_t)(512 * (v + 1) / 100); }

static void mod_laws(const struct r80_program *p, const struct r80_values *v, struct r80_target *t)
{
    extern const uint16_t r80_glide[101];
    struct r80_mod *m = &t->mod;

    m->lfo_inc = rate_inc(p, v, R80_P_LFO_RATE);
    m->lfo_pw = pulse_width(unit(v, R80_P_LFO_PWIDTH));
    m->lfo_dm = (uint16_t)((256 * unit(v, R80_P_LFO_DEPTH) + 50) / 100);
    m->lfo_shape = (uint8_t)unit(v, R80_P_LFO_SHAPE);
    m->env_r[0] = steps(unit(v, R80_P_ENVL_REL));
    m->env_r[1] = steps(unit(v, R80_P_ENVR_REL));
    m->ar_a = steps(unit(v, R80_P_AR_ATTACK));
    m->ar_r = steps(unit(v, R80_P_AR_RELEASE));
    m->ar_mode = (uint8_t)unit(v, R80_P_AR_MODE);
    for (int j = 0; j < 2; j++) {
        uint32_t inc = rate_inc(p, v, j ? R80_P_SW2_RATE : R80_P_SW1_RATE);
        uint16_t pw = pulse_width(unit(v, j ? R80_P_SW2_PWIDTH : R80_P_SW1_PWIDTH));
        m->sw_mode[j] = (uint8_t)unit(v, j ? R80_P_SW2_MODE : R80_P_SW1_MODE);
        m->sw_inc[j] = inc;
        m->sw_pw[j] = pw;
        m->sw_up[j] = (uint16_t)(127u * inc / pw);
        m->sw_down[j] = (uint16_t)(127u * inc / (512u - pw));
    }
    t->post_range = (uint16_t)(3 * v->v[R80_P_RGLDRANGE] / 2 + 1);
    t->voice_range = (uint16_t)(3 * v->v[R80_P_VGLDRANGE] / 2 + 1);
    t->post_resp = r80_glide[unit(v, R80_P_RGLDRESP)];
    t->voice_resp = r80_glide[unit(v, R80_P_VGLDRESP)];
    t->apsweep = p->algo == R80_INVERSE ? 0 : (uint8_t)unit(v, R80_P_APSWEEP);
    t->divider = p->algo == R80_HALL ? r80_divider[unit(v, R80_P_CHORUS)] : 0;
    t->clear = (uint8_t)unit(v, R80_P_CLEAR);
}

void r80_resolve(const struct r80_program *p, const struct r80_values *v, struct r80_target *t, int offsets)
{
    mod_laws(p, v, t);
    tank_k(p, v, t->k);
    if (offsets) {
        tank_a(p, v, t->a);
    }
    if (p->algo == R80_CHORUS) {
        io31(p, v, t->io);
    } else {
        io3(p, v, t->io);
    }
}

/* ---- the Reverb page's fields ---- */

static const uint8_t edit_param[R80_EDITS] = {
    [R80_E_ADJUST] = 0, [R80_E_MIDRT] = R80_P_MIDRT, [R80_E_SIZE] = R80_P_SIZE, [R80_E_PREDELAY] = R80_P_PREDELAY,
    [R80_E_HIGHCUT] = R80_P_HIGHCUT, [R80_E_RTHC] = R80_P_RTHC, [R80_E_LOWRT] = R80_P_LOWRT,
    [R80_E_DIFFUSION] = R80_P_DIFFUSION, [R80_E_ATTACK] = R80_P_ATTACK, [R80_E_SHAPE] = R80_P_SHAPE,
    [R80_E_DURATION] = R80_P_DURATION, [R80_E_LOWSLOPE] = R80_P_LOWSLOPE, [R80_E_MIDSLOPE] = R80_P_MIDSLOPE,
    [R80_E_DEPTH] = R80_P_DEPTH, [R80_E_MSTDEPTH] = R80_P_MSTDEPTH, [R80_E_MSTRATE] = R80_P_MSTRATE,
    [R80_E_DLYMASTER] = R80_P_DLY_MASTER, [R80_E_FBMASTER] = R80_P_FB_MASTER, [R80_E_FXMIX] = R80_P_FXMIX,
};

unsigned r80_edit_param(unsigned e) { return edit_param[e]; }

static const char *const edit_names[R80_EDITS] = {
    "ADJUST", "MID RT", "SIZE", "PRE DLY", "HI CUT", "RT HC", "LOW RT", "DIFFUSE", "ATTACK", "SHAPE", "DURATION",
    "LO SLOPE", "MID SLOPE", "DEPTH", "MST DEPTH", "MST RATE", "DLY MSTR", "FB MSTR", "FX MIX",
};
static const char *const edit_keys[R80_EDITS] = {
    "Adjust", "MidRt", "Size", "PreDelay", "HighCut", "RtHC", "LowRt", "Diffusion", "Attack", "Shape", "Duration",
    "LowSlope", "MidSlope", "Depth", "MstDepth", "MstRate", "DelayMaster", "FbMaster", "FXMix",
};

unsigned r80_fields(const struct r80_program *p, uint8_t edit[R80_FIELDS_MAX])
{
    static const uint8_t lists[R80_ALGOS][R80_FIELDS_MAX] = {
        [R80_CHAMBER] = { R80_E_ADJUST, R80_E_MIDRT, R80_E_SIZE, R80_E_PREDELAY, R80_E_HIGHCUT, R80_E_RTHC,
                          R80_E_LOWRT, R80_E_DIFFUSION, R80_E_SHAPE },
        [R80_PLATE] = { R80_E_ADJUST, R80_E_MIDRT, R80_E_SIZE, R80_E_PREDELAY, R80_E_HIGHCUT, R80_E_RTHC,
                        R80_E_LOWRT, R80_E_DIFFUSION, R80_E_ATTACK },
        [R80_HALL] = { R80_E_ADJUST, R80_E_MIDRT, R80_E_SIZE, R80_E_PREDELAY, R80_E_HIGHCUT, R80_E_RTHC, R80_E_LOWRT,
                       R80_E_DIFFUSION, R80_E_DEPTH },
        [R80_INVERSE] = { R80_E_ADJUST, R80_E_DURATION, R80_E_LOWSLOPE, R80_E_MIDSLOPE, R80_E_SHAPE,
                          R80_E_DIFFUSION, R80_E_PREDELAY, R80_E_HIGHCUT, R80_E_RTHC },
        [R80_CHORUS] = { R80_E_ADJUST, R80_E_MIDRT, R80_E_SIZE, R80_E_HIGHCUT, R80_E_MSTDEPTH, R80_E_MSTRATE,
                         R80_E_DLYMASTER, R80_E_FBMASTER, R80_E_FXMIX },
    };
    memcpy(edit, lists[p->algo], R80_FIELDS_MAX);
    return R80_FIELDS_MAX;
}

const char *r80_edit_name(const struct r80_program *p, unsigned e)
{
    return e == R80_E_ADJUST && p ? p->adjust_label : e < R80_EDITS ? edit_names[e] : "";
}

const char *r80_edit_key(unsigned e)
{
    return e < R80_EDITS ? edit_keys[e] : 0;
}

void r80_edit_range(const struct r80_program *p, unsigned e, int *lo, int *hi)
{
    *lo = 0;
    *hi = e == R80_E_ADJUST ? p->adjust_max : param_max(p, edit_param[e]);
}

int r80_plain(const struct r80_program *p, const struct r80_values *v, unsigned q)
{
    if (r80_locked(v, q)) {
        int t = (tank_time(p, v, q) - 1 + 48) / 96;
        return t > param_max(p, q) ? param_max(p, q) : t;
    }
    return (v->v[q] + 16) >> 5;
}

int r80_edit_base(const struct r80_program *p, unsigned e)
{
    struct r80_values s, v;
    int16_t src[R80_SOURCES];

    if (e == R80_E_ADJUST) {
        return p->adjust;
    }
    for (unsigned i = 0; i < R80_SOURCES; i++) {
        src[i] = 0;
    }
    src[R80_SRC_NONE] = src[R80_SRC_NOVALUE] = -1;
    src[R80_SRC_ON] = 127;
    src[R80_SRC_ADJUST] = p->adjust;
    r80_stored(p, &s);
    r80_patch_all(p, &s, src, &v);
    return r80_plain(p, &v, edit_param[e]);
}

static void fmt_int(char *o, int v)
{
    char t[12];
    int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)-v : (unsigned)v;

    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    if (neg) {
        *o++ = '-';
    }
    while (n) {
        *o++ = t[--n];
    }
    *o = 0;
}

static void cat(char *o, const char *s)
{
    while (*o) {
        o++;
    }
    while ((*o++ = *s++)) {
    }
}

static void fmt_hz(char *o, double f)
{
    if (f >= 10000.0) {
        fmt_int(o, (int)(f / 1000.0 + 0.5));
        cat(o, "K");
    } else if (f >= 1000.0) {
        int t = (int)(f / 100.0 + 0.5);
        fmt_int(o, t / 10);
        cat(o, ".");
        char d[2] = { (char)('0' + t % 10), 0 };
        cat(o, d);
        cat(o, "K");
    } else {
        fmt_int(o, (int)(f + 0.5));
        cat(o, "HZ");
    }
}

void r80_edit_format(const struct r80_program *p, unsigned e, int v, char *o)
{
    unsigned q = edit_param[e];
    int lo, hi;

    o[0] = 0;
    r80_edit_range(p, e, &lo, &hi);
    v = v < lo ? lo : v > hi ? hi : v;
    switch (e) {
    case R80_E_MIDRT: {
        int ms = r80_rt_ms[v], t = ms >= 10000 ? (ms + 50) / 100 : (ms + 5) / 10;
        fmt_int(o, t / (ms >= 10000 ? 10 : 100));
        cat(o, ".");
        char d[3] = { 0 };
        if (ms >= 10000) {
            d[0] = (char)('0' + t % 10);
        } else {
            d[0] = (char)('0' + t / 10 % 10);
            d[1] = (char)('0' + t % 10);
        }
        cat(o, d);
        cat(o, " S");
        return;
    }
    case R80_E_PREDELAY:
        fmt_int(o, 2 * v);
        cat(o, " MS");
        return;
    case R80_E_HIGHCUT:
        if (v >= 121) {
            cat(o, "OFF");
        } else {
            fmt_hz(o, -m_log(1.0 - r80_hicut[v] / 8388608.0) * FS / (2.0 * PI));
        }
        return;
    case R80_E_RTHC:
        if (v >= 48) {
            cat(o, "FULL");
        } else {
            fmt_hz(o, r80_damp_hz[v + 12]);
        }
        return;
    case R80_E_LOWRT: {
        static const char *const m[10] = { "X0.2", "X0.4", "X0.6", "X0.8", "X1.0", "X1.2", "X1.5", "X2.0", "X3.0",
                                           "X4.0" };
        cat(o, m[v]);
        return;
    }
    case R80_E_DIFFUSION:
        if (!v) {
            cat(o, "OFF");
        } else if (v == hi) {
            cat(o, "FULL");
        } else {
            fmt_int(o, 100 * v / hi);
            cat(o, "%");
        }
        return;
    case R80_E_LOWSLOPE:
    case R80_E_MIDSLOPE:
        fmt_int(o, v - 16);
        return;
    case R80_E_MSTDEPTH:
    case R80_E_MSTRATE:
    case R80_E_DLYMASTER:
    case R80_E_FBMASTER:
    case R80_E_FXMIX:
        fmt_int(o, v);
        cat(o, "%");
        return;
    default:
        (void)q;
        fmt_int(o, v);
        return;
    }
}
