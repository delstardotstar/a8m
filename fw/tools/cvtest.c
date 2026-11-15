/* Host check of src/cvframe.c (AArch64 host): a scalar model of the CV
 * value chain against the NEON code over random scans, and worked numbers.
 * usage: tools/cvtest.sh */
#include "cvframe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int sat(long v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int)v;
}

static long fdiv(long a, long d)
{
    return a >= 0 ? a / d : -((-a + d - 1) / d);
}

static int q15(int a, int b)
{
    return sat(fdiv((long)a * b + (1 << 14), 1 << 15));
}

static int shr(int v, int n)
{
    return (int)fdiv(v, 1L << n);
}

static const int ke[4] = { 3275, 11185, 20479, 30108 }, kl[4] = { 856, 6874, 15800, 25188 };

struct chain {
    int up, y[4];
};

static int allpass(struct chain *c, const int *k, int u)
{
    int in = u, pin = c->up;

    for (int j = 0; j < 4; j++) {
        int y = sat(q15(sat(in - c->y[j]), k[j]) + pin);
        pin = c->y[j];
        c->y[j] = y;
        in = y;
    }
    c->up = u;
    return in;
}

struct model {
    struct chain e[4], l[4];
    int amean[4];
};

static void model_frame(struct model *m, const int16_t *off, const int16_t *gain, const int16_t *r, int out[3][4])
{
    int v[3][4], w[3][4];

    for (int k = 0; k < 4; k++) {
        int ye = allpass(&m->e[k], ke, sat(r[k] + off[k]));
        int yl = allpass(&m->l[k], kl, sat(r[8 + k] + off[k]));
        m->amean[k] = (int)fdiv(ye + yl + 1, 2);
        v[0][k] = q15(m->amean[k], gain[k]);
        v[1][k] = q15(sat(r[4 + k] + off[8 + k]), gain[8 + k]);
        v[2][k] = q15(sat(r[12 + k] + off[16 + k]), gain[16 + k]);
    }
    for (int g = 0; g < 3; g++) {
        w[g][0] = v[g][0];
        for (int k = 1; k < 4; k++) {
            w[g][k] = sat(v[g][k] - shr(v[g][k - 1], 10));
        }
    }
    w[1][0] = sat(v[1][0] - shr(w[0][3], 10));
    w[2][0] = sat(v[2][0] - shr(w[0][3], 10));
    w[0][0] = sat(v[0][0] - shr(sat(w[1][3] + w[2][3]), 11));
    for (int g = 0; g < 3; g++) {
        for (int k = 0; k < 4; k++) {
            out[g][k] = sat(4L * w[g][k]);
        }
    }
}

/* ADC 0 of tmp/sd0.img's cal.bin (stock order, 1A..8A, 1B..8B, 1C..8C). */
static const int16_t cal_off[24] = { -16343, -16249, -16344, -16328, -16305, -16337, -16312, -16288,
                                     -16327, -16336, -16327, -16304, -16327, -16358, -16294, -16298,
                                     -16321, -16376, -16312, -16240, -16288, -16304, -16352, -16313 };
static const int16_t cal_gain[24] = { -17908, -17954, -17918, -17954, -17991, -17958, -17947, -17536,
                                      -17979, -17912, -17924, -17942, -17904, -17918, -17917, -17990,
                                      -17949, -17896, -17978, -17950, -17536, -17947, -17942, -17925 };

static void fill(int16_t r[16], int code)
{
    for (int i = 0; i < 16; i++) {
        r[i] = (int16_t)(code << 3);
    }
}

/* All inputs at code 2048 for a long time, then 1B at 1854 (description
 * section 7): 4A's filter alternates 59 / 54, 1B reads 3284. */
static void worked(void)
{
    static int16_t r[AUDIO_BLOCK][16];
    static int16_t out[3][AUDIO_BLOCK][4];
    struct cv_adc_state s;
    struct model m;
    int o[3][4], seen59 = 0, seen54 = 0;

    memset(&s, 0, sizeof(s));
    memset(&m, 0, sizeof(m));
    for (int f = 0; f < AUDIO_BLOCK; f++) {
        fill(r[f], 2048);
        r[f][4] = 1854 << 3;
    }
    for (int b = 0; b < 200 * BLOCKS_PER_32; b++) {
        cv_adc_frames(&s, cal_off, cal_gain, (const int16_t (*)[16])r, AUDIO_BLOCK, out);
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            model_frame(&m, cal_off, cal_gain, r[f], o);
            for (int g = 0; g < 3; g++) {
                for (int k = 0; k < 4; k++) {
                    CHECK(o[g][k] == out[g][f][k], "worked block %d frame %d g %d k %d: model %d neon %d", b, f, g,
                          k, o[g][k], out[g][f][k]);
                }
            }
            if (b == 200 * BLOCKS_PER_32 - 1) {
                seen59 += m.amean[3] == 59;
                seen54 += m.amean[3] == 54;
                CHECK(o[1][0] == 3284, "1B = %d, want 3284", o[1][0]);
            }
        }
    }
    CHECK(seen59 == AUDIO_BLOCK / 2 && seen54 == AUDIO_BLOCK / 2, "4A filter: %d x 59, %d x 54 (want alternating)", seen59, seen54);
    printf("worked: 4A filter 59/54 alternating, 1B = %d (3280 without the previous-conversion step)\n",
           out[1][AUDIO_BLOCK - 1][0]);
}

/* Filter alone at x = 1000: 1005, 996, ... (and 56 -> 59/54, -1495 -> -1499/-1490). */
static void limit_cycle(int x, int a, int b)
{
    struct model m;
    int16_t r[16], off[24] = { 0 }, gain[24];
    int o[3][4], last[2] = { 0, 0 };

    memset(&m, 0, sizeof(m));
    for (int i = 0; i < 24; i++) {
        gain[i] = -32768;
    }
    fill(r, 0);
    for (int k = 0; k < 4; k++) {
        r[k] = r[8 + k] = (int16_t)x;
    }
    for (int f = 0; f < 4000; f++) {
        model_frame(&m, off, gain, r, o);
        last[f & 1] = m.amean[0];
    }
    int lo = last[0] < last[1] ? last[0] : last[1], hi = last[0] ^ last[1] ^ lo;
    CHECK(lo == a && hi == b, "x = %d: %d / %d, want %d / %d", x, lo, hi, a, b);
    printf("limit cycle x = %d: %d / %d\n", x, hi, lo);
}

static int code(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

/* Random scans (static, noise, full-scale steps) through both. */
static void random_runs(void)
{
    static int16_t r[AUDIO_BLOCK][16];
    static int16_t out[3][AUDIO_BLOCK][4];
    struct cv_adc_state s;
    struct model m;
    int16_t off[24], gain[24];
    int o[3][4];
    long n = 0;

    memset(&s, 0, sizeof(s));
    memset(&m, 0, sizeof(m));
    for (int run = 0; run < 400; run++) {
        int kind = run % 4;
        for (int i = 0; i < 24; i++) {
            off[i] = run < 200 ? cal_off[i] : (int16_t)code(-32768, 32767);
            gain[i] = run < 200 ? cal_gain[i] : (int16_t)code(-32768, 32767);
        }
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            for (int i = 0; i < 16; i++) {
                int c = kind == 0 ? code(0, 4095) : kind == 1 ? (rand() & 1) * 4095 : kind == 2 ? 2048 + code(-3, 3)
                                                                                                : code(1500, 2600);
                r[f][i] = (int16_t)(c << 3);
            }
        }
        cv_adc_frames(&s, off, gain, (const int16_t (*)[16])r, AUDIO_BLOCK, out);
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            model_frame(&m, off, gain, r[f], o);
            for (int g = 0; g < 3; g++) {
                for (int k = 0; k < 4; k++) {
                    n++;
                    if (o[g][k] != out[g][f][k]) {
                        CHECK(0, "random run %d frame %d g %d k %d: model %d neon %d", run, f, g, k, o[g][k],
                              out[g][f][k]);
                        return;
                    }
                }
            }
        }
    }
    printf("random: %ld values equal\n", n);
}

static const int seq[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 8, 9, 10, 11 };

#define SCANS 128
static uint16_t ring[16][SCANS] __attribute__((aligned(16)));

/* ring row b holds sequence position (b - rot) & 15 of every scan. */
static void fill_ring(int rot, const int code[16])
{
    for (int b = 0; b < 16; b++) {
        int p = (b - rot) & 15;
        for (int c = 0; c < SCANS; c++) {
            ring[b][c] = (uint16_t)(seq[p] << 12 | code[p]);
        }
    }
}

/* Every rotation; one bad word (wrong ID, zero, never written) at each
 * position: the rest keep their rows, a bad CV A sample takes the input's
 * other one; too few IDs keep the map. */
static void rowmaps(void)
{
    static const uint16_t junk[3] = { 0x9800, 0x0000, 0xffff };
    uint16_t w[16];
    struct cv_rowmap m;
    long n = 0;

    for (int rot = 0; rot < 16; rot++) {
        for (int bad = -1; bad < 16; bad++) {
            for (int j = 0; j < 3; j++) {
                cv_rowmap_init(&m);
                m.rot = (uint8_t)((rot + 5) & 15);
                for (int b = 0; b < 16; b++) {
                    w[b] = (uint16_t)(seq[(b - rot) & 15] << 12 | code(0, 4095));
                }
                int bp = bad < 0 ? -1 : (bad + rot) & 15;
                if (bp >= 0) {
                    if (seq[bad] == (junk[j] >> 12)) {
                        continue;
                    }
                    w[bp] = junk[j];
                }
                CHECK(cv_rowmap_update(&m, w), "rot %d bad %d: no map", rot, bad);
                CHECK(m.rot == rot, "rot %d bad %d: found %d", rot, bad, m.rot);
                CHECK(m.bad == (bp < 0 ? 0 : 1u << bp), "rot %d bad %d: bad %04x", rot, bad, m.bad);
                for (int p = 0; p < 16; p++) {
                    int want = (p + rot) & 15;
                    if (p == bad && (p < 4 || (p >= 8 && p < 12))) {
                        want = ((p ^ 8) + rot) & 15;
                    }
                    CHECK(m.row[p] == want, "rot %d bad %d: row[%d] = %d, want %d", rot, bad, p, m.row[p], want);
                    n++;
                }
            }
        }
    }
    cv_rowmap_init(&m);
    for (int b = 0; b < 16; b++) {
        w[b] = (uint16_t)(seq[(b - 3) & 15] << 12);
    }
    cv_rowmap_update(&m, w);
    for (int b = 0; b < 16; b++) {
        w[b] = b < 6 ? 0xffff : (uint16_t)(seq[(b - 7) & 15] << 12);
    }
    CHECK(!cv_rowmap_update(&m, w) && m.rot == 3, "6 junk words moved the map to %d", m.rot);
    printf("row maps: %ld rows right\n", n);
}

/* cv_ring_frames against a scalar read for random spans and row orders. */
static void ring_reads(void)
{
    int16_t r[AUDIO_BLOCK][16];
    long n = 0;

    for (int it = 0; it < 20000; it++) {
        const uint16_t *row[16];
        int perm[16];
        for (int b = 0; b < 16; b++) {
            for (int c = 0; c < SCANS; c++) {
                ring[b][c] = (uint16_t)rand();
            }
        }
        for (int p = 0; p < 16; p++) {
            perm[p] = rand() % 16;
            row[p] = ring[perm[p]];
        }
        float rd = (float)(rand() % (SCANS * 1000)) / 1000.0f, rate = 0.5f + (float)(rand() % 1000) / 1000.0f;
        uint32_t q0 = (uint32_t)((rd + 0.5f) * 65536.0f), dq = (uint32_t)(rate * 65536.0f);
        unsigned s0 = q0 >> 16, ns = ((q0 + (AUDIO_BLOCK - 1) * dq) >> 16) - s0 + 1;
        ns = ns > CV_RING_SPAN ? CV_RING_SPAN : ns;
        cv_ring_frames(row, SCANS, s0, ns, q0, dq, r, AUDIO_BLOCK);
        for (unsigned f = 0; f < AUDIO_BLOCK; f++) {
            unsigned k = ((q0 + f * dq) >> 16) - s0;
            k = k >= ns ? ns - 1 : k;
            for (int p = 0; p < 16; p++) {
                int want = (ring[perm[p]][(s0 + k) % SCANS] & 0xfff) << 3;
                if (r[f][p] != want) {
                    CHECK(0, "ring it %d frame %u pos %d: %d, want %d", it, f, p, r[f][p], want);
                    return;
                }
                n++;
            }
        }
    }
    printf("ring reads: %ld values equal\n", n);
}

/* ADC 1 (inputs 5-8) at 0 V through the ring with a bad word in 6A's later
 * sample: 6A reads as with a clean scan (hardware: +2.73 V before, when
 * the unmatched sample stayed at code 0). */
static void bad_word_6a(void)
{
    static int16_t r[AUDIO_BLOCK][16], out[3][AUDIO_BLOCK][4], ref[3][AUDIO_BLOCK][4];
    int code0[16];
    struct cv_adc_state s, sr;
    struct cv_rowmap m, mr;
    const uint16_t *row[16];
    uint16_t w[16];
    int rot = 1;

    for (int p = 0; p < 16; p++) {
        code0[p] = 2048;
    }
    memset(&s, 0, sizeof(s));
    memset(&sr, 0, sizeof(sr));
    for (int pass = 0; pass < 2; pass++) {
        fill_ring(rot, code0);
        if (pass) {
            for (int c = 0; c < SCANS; c++) {
                ring[(9 + rot) & 15][c] = 0x9800;
            }
        }
        struct cv_rowmap *mp = pass ? &m : &mr;
        cv_rowmap_init(mp);
        for (int b = 0; b < 16; b++) {
            w[b] = ring[b][0];
        }
        cv_rowmap_update(mp, w);
        for (int p = 0; p < 16; p++) {
            row[p] = ring[mp->row[p]];
        }
        for (int blk = 0; blk < 100; blk++) {
            cv_ring_frames(row, SCANS, 0, AUDIO_BLOCK, 0, 65536, r, AUDIO_BLOCK);
            cv_adc_frames(pass ? &s : &sr, cal_off + 4, cal_gain + 4, (const int16_t (*)[16])r, AUDIO_BLOCK,
                          pass ? out : ref);
        }
    }
    long sum = 0, sum_ref = 0;
    for (int f = 0; f < AUDIO_BLOCK; f++) {
        sum += out[0][f][1];
        sum_ref += ref[0][f][1];
    }
    CHECK(m.bad == 1u << ((9 + rot) & 15), "6A bad word not flagged: %04x", m.bad);
    CHECK(labs(sum - sum_ref) <= 20 * AUDIO_BLOCK, "6A with a bad word: mean %ld, clean %ld", sum / AUDIO_BLOCK,
          sum_ref / AUDIO_BLOCK);
    printf("6A bad word: mean %ld (clean %ld)\n", sum / AUDIO_BLOCK, sum_ref / AUDIO_BLOCK);
}

int main(void)
{
    worked();
    limit_cycle(1000, 996, 1005);
    limit_cycle(56, 54, 59);
    limit_cycle(-1495, -1499, -1490);
    random_runs();
    rowmaps();
    ring_reads();
    bad_word_6a();
    printf(fails ? "FAILED\n" : "ok\n");
    return fails != 0;
}
