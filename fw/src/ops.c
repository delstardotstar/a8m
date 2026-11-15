#include "ops.h"

#include <string.h>

#define HOP_MIN     256u
#define HOPS_MAX    8192u
#define MIN_STRENGTH 0.25f

void slices_equal(uint32_t start, uint32_t end, int n, uint32_t *b)
{
    uint64_t len = end - start;

    for (int k = 0; k <= n; k++) {
        b[k] = start + (uint32_t)(len * (uint32_t)k / (uint32_t)n);
    }
}

static float log2_fast(float x)
{
    union { float f; uint32_t u; } v = { x };
    float m;
    int e = (int)(v.u >> 23) - 127;

    v.u = (v.u & 0x7fffffu) | 0x3f800000u;
    m = v.f;
    return (float)e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

static float mono(const int32_t *l, const int32_t *r, uint32_t i)
{
    return l == r ? (float)l[i] : 0.5f * ((float)l[i] + (float)r[i]);
}

static float hop_energy(const int32_t *l, const int32_t *r, uint32_t a, uint32_t n)
{
    float prev = mono(l, r, a), sum = 0.0f;

    for (uint32_t i = 1; i < n; i++) {
        float x = mono(l, r, a + i);
        sum += __builtin_fabsf(x - prev);
        prev = x;
    }
    return sum / (float)(n > 1 ? n - 1 : 1);
}

static uint32_t refine(const int32_t *l, const int32_t *r, uint32_t start, uint32_t end, uint32_t p, uint32_t hop)
{
    uint32_t a = p > start + hop ? p - hop : start + 1, e = p + 2 * hop < end ? p + 2 * hop : end;
    float top = 0.0f;

    for (uint32_t i = a; i < e; i++) {
        float d = __builtin_fabsf(mono(l, r, i) - mono(l, r, i - 1));
        top = d > top ? d : top;
    }
    for (uint32_t i = a; i < e; i++) {
        if (__builtin_fabsf(mono(l, r, i) - mono(l, r, i - 1)) >= 0.25f * top) {
            return i;
        }
    }
    return p;
}

/* w[0..have+1] ascending, ends included; halves the longest slice until
 * there are n. */
static void fill_gaps(uint32_t *w, int have, int n)
{
    while (have + 1 < n) {
        int at = 0;
        uint32_t best = 0;

        for (int k = 0; k <= have; k++) {
            if (w[k + 1] - w[k] > best) {
                best = w[k + 1] - w[k];
                at = k;
            }
        }
        for (int k = have + 2; k > at + 1; k--) {
            w[k] = w[k - 1];
        }
        w[at + 1] = w[at] + best / 2;
        have++;
    }
}

void slices_chop(const int32_t *l, const int32_t *r, uint32_t start, uint32_t end, uint32_t rate, int n,
                 uint32_t *b)
{
    static float lg[HOPS_MAX], o[HOPS_MAX];
    static uint16_t cand[HOPS_MAX];
    uint32_t len = end - start, hop = HOP_MIN, nh, gap;
    float peak = 0.0f, floor_;
    uint32_t w[SLICES_MAX + 2];
    int nc = 0, have = 0;

    b[0] = start;
    b[n] = end;
    while (len / hop > HOPS_MAX) {
        hop *= 2;
    }
    nh = len / hop;
    if (n < 2 || nh < 8u * (uint32_t)n) {
        slices_equal(start, end, n, b);
        return;
    }
    for (uint32_t h = 0; h < nh; h++) {
        uint32_t w = hop < HOP_MIN ? hop : HOP_MIN;
        lg[h] = hop_energy(l, r, start + h * hop, w);
        peak = lg[h] > peak ? lg[h] : peak;
    }
    if (peak <= 0.0f) {
        slices_equal(start, end, n, b);
        return;
    }
    floor_ = peak * 1e-4f;
    for (uint32_t h = 0; h < nh; h++) {
        lg[h] = log2_fast(lg[h] + floor_);
    }
    for (uint32_t h = 0; h < nh; h++) {
        float m = 0.0f;
        int k = 0;

        for (uint32_t j = h >= 3 ? h - 3 : 0; j < h; j++, k++) {
            m += lg[j];
        }
        o[h] = k ? lg[h] - m / (float)k : 0.0f;
        o[h] = o[h] > 0.0f ? o[h] : 0.0f;
    }
    gap = rate * 30u / 1000u / hop;
    gap = gap < 2 ? 2 : gap;
    if (nh / (8u * (uint32_t)n) > gap) {
        gap = nh / (8u * (uint32_t)n);
    }
    for (uint32_t h = gap; h + gap < nh; h++) {
        if (o[h] >= MIN_STRENGTH && o[h] >= o[h - 1] && o[h] > o[h + 1]) {
            cand[nc++] = (uint16_t)h;
        }
    }
    while (have < n - 1 && nc) {
        int best = -1;
        uint32_t at;

        for (int c = 0; c < nc; c++) {
            if (cand[c] != 0xffffu && (best < 0 || o[cand[c]] > o[cand[best]])) {
                best = c;
            }
        }
        if (best < 0) {
            break;
        }
        at = cand[best];
        for (int c = 0; c < nc; c++) {
            if (cand[c] != 0xffffu && (cand[c] > at ? cand[c] - at : at - cand[c]) < gap) {
                cand[c] = 0xffffu;
            }
        }
        w[1 + have++] = refine(l, r, start, end, start + at * hop, hop);
    }
    for (int i = 2; i <= have; i++) {
        uint32_t v = w[i];
        int k = i - 1;
        for (; k > 0 && w[k] > v; k--) {
            w[k + 1] = w[k];
        }
        w[k + 1] = v;
    }
    w[0] = start;
    w[have + 1] = end;
    fill_gaps(w, have, n);
    for (int k = 1; k < n; k++) {
        if (w[k] < w[k - 1] + 4u || w[k] + 4u > end) {
            slices_equal(start, end, n, b);
            return;
        }
    }
    memcpy(b, w, sizeof(uint32_t) * (unsigned)(n + 1));
}

void zone_trim(struct zone_cfg *z, uint32_t from, uint32_t to, uint32_t old_frames)
{
    int64_t len = (int64_t)to - from, s, e, ls, le32;
    int64_t old_end = z->end == POS_END || z->end > old_frames ? old_frames : z->end;
    int64_t old_le32 = z->loop_len == POS_END ? (int64_t)old_frames * 32 : (int64_t)z->loop_start * 32 + z->loop_len;

    old_le32 = old_le32 > (int64_t)old_frames * 32 ? (int64_t)old_frames * 32 : old_le32;
    s = (int64_t)z->start - from;
    s = s < 0 ? 0 : s > len - 1 ? len - 1 : s;
    e = old_end - from;
    e = e < s + 1 ? s + 1 : e;
    z->start = (uint32_t)s;
    z->end = e >= len ? POS_END : (uint32_t)e;
    ls = (int64_t)z->loop_start - from;
    ls = ls < 0 ? 0 : ls > len - 4 ? len - 4 : ls;
    le32 = old_le32 - (int64_t)from * 32;
    le32 = le32 < (ls + 4) * 32 ? (ls + 4) * 32 : le32;
    z->loop_start = (uint32_t)ls;
    z->loop_len = le32 >= len * 32 ? POS_END : (uint32_t)(le32 - ls * 32);
}

int zones_count(const struct sampler_cfg *s)
{
    int n = 0;

    while (n < ZONES && s->zone[n].smp) {
        n++;
    }
    return n;
}

static void zones_close(struct sampler_cfg *s)
{
    int n = zones_count(s);

    if (n) {
        s->zone[n - 1].minv = -500;
    }
}

int zones_append(struct sampler_cfg *s, const struct zone_cfg *z)
{
    int n = zones_count(s);

    if (n >= ZONES) {
        return -1;
    }
    if (n) {
        int top = n >= 2 ? s->zone[n - 2].minv : 500;
        s->zone[n - 1].minv = (int16_t)((top - 500) / 2);
    }
    s->zone[n] = *z;
    s->zone[n].minv = -500;
    return n;
}

void zones_delete(struct sampler_cfg *s, int z)
{
    if (z < 0 || z >= zones_count(s)) {
        return;
    }
    for (int k = z; k + 1 < ZONES; k++) {
        s->zone[k] = s->zone[k + 1];
    }
    zone_defaults(&s->zone[ZONES - 1]);
    zones_close(s);
}

void zones_insert(struct sampler_cfg *s, int z)
{
    int n = zones_count(s), above, own;

    if (z < 0 || z >= n) {
        return;
    }
    above = z ? s->zone[z - 1].minv : 500;
    own = z + 1 < n ? s->zone[z].minv : -500;
    for (int k = n < ZONES ? n : ZONES - 1; k > z; k--) {
        s->zone[k] = s->zone[k - 1];
    }
    s->zone[z].minv = (int16_t)((above + own) / 2);
    zones_close(s);
}

void zones_flip(struct sampler_cfg *s, int z, int n)
{
    if (z < 0 || n < 2 || z + n > zones_count(s)) {
        return;
    }
    for (int a = z, b = z + n - 1; a < b; a++, b--) {
        struct zone_cfg t = s->zone[a];
        int16_t ma = s->zone[a].minv, mb = s->zone[b].minv;

        s->zone[a] = s->zone[b];
        s->zone[b] = t;
        s->zone[a].minv = ma;
        s->zone[b].minv = mb;
    }
}

void zone_follow(struct zone_cfg *r, const struct zone_cfg *m, int stereo)
{
    *r = *m;
    r->side = (uint8_t)(stereo && !m->side);
}

int zones_sr_follow(const struct sampler_cfg *m, struct sampler_cfg *r, int z, int old, int n, int stereo)
{
    int rn = zones_count(r);
    struct zone_cfg f;

    if (z >= zones_count(m) || !(stereo ? z <= rn : z < n ? z < rn && r->zone[z].smp == old : rn == n)) {
        return 0;
    }
    zone_follow(&f, &m->zone[z], stereo);
    if (z < rn) {
        r->zone[z] = f;
    } else if (zones_append(r, &f) < 0) {
        return 0;
    }
    return 1;
}
