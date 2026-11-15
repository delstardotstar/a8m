#include "cvframe.h"

#include <arm_neon.h>

static const int16_t hb_k[4][8] = {
    { 3275, 3275, 3275, 3275, 856, 856, 856, 856 },
    { 11185, 11185, 11185, 11185, 6874, 6874, 6874, 6874 },
    { 20479, 20479, 20479, 20479, 15800, 15800, 15800, 15800 },
    { 30108, 30108, 30108, 30108, 25188, 25188, 25188, 25188 },
};

/* Removes 1/1024 of the previous conversion on the same ADC (floor shifts):
 * within a group from the input before; the first B and C from the fourth
 * A, the first A from half of each of the fourth B and C. */
static inline void prev_conv(int16x4_t *a, int16x4_t *b, int16x4_t *c)
{
    const int16x4_t z = vdup_n_s16(0), first = { -1, 0, 0, 0 };
    int16x4_t wa = vqsub_s16(*a, vshr_n_s16(vext_s16(z, *a, 3), 10));
    int16x4_t wb = vqsub_s16(*b, vshr_n_s16(vext_s16(z, *b, 3), 10));
    int16x4_t wc = vqsub_s16(*c, vshr_n_s16(vext_s16(z, *c, 3), 10));
    int16x4_t a4 = vand_s16(vshr_n_s16(vdup_lane_s16(wa, 3), 10), first);
    int16x4_t bc = vand_s16(vshr_n_s16(vqadd_s16(vdup_lane_s16(wb, 3), vdup_lane_s16(wc, 3)), 11), first);

    *a = vqsub_s16(wa, bc);
    *b = vqsub_s16(wb, a4);
    *c = vqsub_s16(wc, a4);
}

void cv_adc_frames(struct cv_adc_state *s, const int16_t *off, const int16_t *gain, const int16_t (*r)[16],
                   unsigned nf, int16_t (*out)[AUDIO_BLOCK][4])
{
    const int16x4_t oa = vld1_s16(off), ob = vld1_s16(off + 8), oc = vld1_s16(off + 16);
    const int16x4_t ga = vld1_s16(gain), gb = vld1_s16(gain + 8), gc = vld1_s16(gain + 16);
    const int16x8_t oaa = vcombine_s16(oa, oa);
    const int16x8_t k0 = vld1q_s16(hb_k[0]), k1 = vld1q_s16(hb_k[1]), k2 = vld1q_s16(hb_k[2]),
                    k3 = vld1q_s16(hb_k[3]);
    int16x8_t up = vld1q_s16(s->up), y0 = vld1q_s16(s->y[0]), y1 = vld1q_s16(s->y[1]), y2 = vld1q_s16(s->y[2]),
              y3 = vld1q_s16(s->y[3]);

    for (unsigned f = 0; f < nf; f++) {
        int16x4_t xb = vqadd_s16(vld1_s16(r[f] + 4), ob), xc = vqadd_s16(vld1_s16(r[f] + 12), oc);
        int16x8_t u = vqaddq_s16(vcombine_s16(vld1_s16(r[f]), vld1_s16(r[f] + 8)), oaa);
        int16x8_t n0 = vqaddq_s16(vqrdmulhq_s16(vqsubq_s16(u, y0), k0), up);
        int16x8_t n1 = vqaddq_s16(vqrdmulhq_s16(vqsubq_s16(n0, y1), k1), y0);
        int16x8_t n2 = vqaddq_s16(vqrdmulhq_s16(vqsubq_s16(n1, y2), k2), y1);
        int16x8_t n3 = vqaddq_s16(vqrdmulhq_s16(vqsubq_s16(n2, y3), k3), y2);
        up = u;
        y0 = n0;
        y1 = n1;
        y2 = n2;
        y3 = n3;
        int16x4_t va = vqrdmulh_s16(vrhadd_s16(vget_low_s16(n3), vget_high_s16(n3)), ga);
        int16x4_t vb = vqrdmulh_s16(xb, gb), vc = vqrdmulh_s16(xc, gc);
        prev_conv(&va, &vb, &vc);
        vst1_s16(out[0][f], vqshl_n_s16(va, 2));
        vst1_s16(out[1][f], vqshl_n_s16(vb, 2));
        vst1_s16(out[2][f], vqshl_n_s16(vc, 2));
    }
    vst1q_s16(s->up, up);
    vst1q_s16(s->y[0], y0);
    vst1q_s16(s->y[1], y1);
    vst1q_s16(s->y[2], y2);
    vst1q_s16(s->y[3], y3);
}

static const uint8_t seq[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 8, 9, 10, 11 };

void cv_rowmap_init(struct cv_rowmap *m)
{
    for (int p = 0; p < 16; p++) {
        m->row[p] = (int8_t)p;
    }
    m->rot = 0;
    m->bad = 0;
}

static uint16_t mismatches(const uint16_t w[16], unsigned rot)
{
    uint16_t bad = 0;

    for (unsigned b = 0; b < 16; b++) {
        unsigned id = w[b] >> 12;
        bad |= (uint16_t)((w[b] == 0xffffu || id != seq[(b - rot) & 15]) << b);
    }
    return bad;
}

int cv_rowmap_update(struct cv_rowmap *m, const uint16_t w[16])
{
    unsigned rot = m->rot;
    uint16_t bad = mismatches(w, rot);

    if (__builtin_popcount(bad) > 4) {
        for (unsigned r = 0; r < 16; r++) {
            uint16_t b = mismatches(w, r);
            if (__builtin_popcount(b) < __builtin_popcount(bad)) {
                rot = r;
                bad = b;
            }
        }
        if (__builtin_popcount(bad) > 4) {
            return 0;
        }
    }
    m->rot = (uint8_t)rot;
    m->bad = bad;
    for (unsigned p = 0; p < 16; p++) {
        m->row[p] = (int8_t)((p + rot) & 15);
    }
    for (unsigned k = 0; k < 4; k++) {
        int e = bad >> m->row[k] & 1, l = bad >> m->row[8 + k] & 1;
        if (e && !l) {
            m->row[k] = m->row[8 + k];
        } else if (l && !e) {
            m->row[8 + k] = m->row[k];
        }
    }
    return 1;
}

/* dst[k * ds + j] = row[j][col + k], an 8 x 8 block. */
static inline void tr8(uint16_t *dst, unsigned ds, const uint16_t *const *row, unsigned col)
{
#define R(j) vld1q_u16(row[j] + col)
    uint16x8x2_t a = vtrnq_u16(R(0), R(1));
    uint16x8x2_t b = vtrnq_u16(R(2), R(3));
    uint16x8x2_t c = vtrnq_u16(R(4), R(5));
    uint16x8x2_t d = vtrnq_u16(R(6), R(7));
#undef R
    uint32x4x2_t e = vtrnq_u32(vreinterpretq_u32_u16(a.val[0]), vreinterpretq_u32_u16(b.val[0]));
    uint32x4x2_t f = vtrnq_u32(vreinterpretq_u32_u16(a.val[1]), vreinterpretq_u32_u16(b.val[1]));
    uint32x4x2_t g = vtrnq_u32(vreinterpretq_u32_u16(c.val[0]), vreinterpretq_u32_u16(d.val[0]));
    uint32x4x2_t h = vtrnq_u32(vreinterpretq_u32_u16(c.val[1]), vreinterpretq_u32_u16(d.val[1]));
    vst1q_u16(dst, vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(e.val[0]), vget_low_u32(g.val[0]))));
    vst1q_u16(dst + ds, vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(f.val[0]), vget_low_u32(h.val[0]))));
    vst1q_u16(dst + 2 * ds, vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(e.val[1]), vget_low_u32(g.val[1]))));
    vst1q_u16(dst + 3 * ds, vreinterpretq_u16_u32(vcombine_u32(vget_low_u32(f.val[1]), vget_low_u32(h.val[1]))));
    vst1q_u16(dst + 4 * ds, vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(e.val[0]), vget_high_u32(g.val[0]))));
    vst1q_u16(dst + 5 * ds, vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(f.val[0]), vget_high_u32(h.val[0]))));
    vst1q_u16(dst + 6 * ds, vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(e.val[1]), vget_high_u32(g.val[1]))));
    vst1q_u16(dst + 7 * ds, vreinterpretq_u16_u32(vcombine_u32(vget_high_u32(f.val[1]), vget_high_u32(h.val[1]))));
}

void cv_ring_frames(const uint16_t *const row[16], unsigned scans, unsigned s0, unsigned n, uint32_t q0, uint32_t dq,
                    int16_t (*r)[16], unsigned nf)
{
    uint16_t s[CV_RING_SPAN + 8][16] __attribute__((aligned(16)));
    unsigned lead = s0 % 8, base = s0 - lead;
    const uint16x8_t m = vdupq_n_u16(0xfff);

    for (unsigned k = 0; k < lead + n; k += 8) {
        unsigned col = (base + k) % scans;
        tr8(&s[k][0], 16, row, col);
        tr8(&s[k][8], 16, row + 8, col);
    }
    for (unsigned f = 0; f < nf; f++) {
        unsigned k = ((q0 + f * dq) >> 16) - s0;
        k = (k >= n ? n - 1 : k) + lead;
        vst1q_s16(r[f], vreinterpretq_s16_u16(vshlq_n_u16(vandq_u16(vld1q_u16(s[k]), m), 3)));
        vst1q_s16(r[f] + 8, vreinterpretq_s16_u16(vshlq_n_u16(vandq_u16(vld1q_u16(s[k] + 8), m), 3)));
    }
}
