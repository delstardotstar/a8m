#include "wave.h"

#define OV_N        4096
#define OV_SLOTS    4
#define DIRECT_MAX  16384u          /* spans up to this are read frame by frame */
#define BUILD_STEP  (1u << 19)      /* overview frames scanned per call */

struct ov {
    const int32_t *x;
    uint32_t frames, crc, done, used;
    int16_t mn[OV_N], mx[OV_N];
};

static struct ov ov[OV_SLOTS];
static uint32_t ov_clock;

int wave_at(const struct sample *s, int side, int64_t f)
{
    return f >= 0 && f < s->frames ? s->plane[side & 1][f] >> 16 : 0;
}

/* Keyed by plane, length and data CRC: the pool is reused per folder. */
static struct ov *ov_get(const struct sample *s, int side)
{
    const int32_t *x = s->plane[side & 1];
    struct ov *o = &ov[0];

    for (int i = 0; i < OV_SLOTS; i++) {
        if (ov[i].x == x && ov[i].frames == s->frames && ov[i].crc == s->crc) {
            ov[i].used = ++ov_clock;
            return &ov[i];
        }
        if (ov[i].used < o->used) {
            o = &ov[i];
        }
    }
    o->x = x;
    o->frames = s->frames;
    o->crc = s->crc;
    o->done = 0;
    o->used = ++ov_clock;
    return o;
}

static int ov_build(struct ov *o)
{
    uint32_t budget = BUILD_STEP;

    while (o->done < OV_N && budget) {
        uint32_t b = o->done;
        uint32_t a = (uint32_t)((uint64_t)b * o->frames / OV_N);
        uint32_t e = (uint32_t)((uint64_t)(b + 1) * o->frames / OV_N);
        int32_t lo, hi;

        if (e <= a) {
            e = a + 1 < o->frames ? a + 1 : o->frames;
            a = e ? e - 1 : 0;
        }
        lo = hi = o->x[a];
        for (uint32_t f = a + 1; f < e; f++) {
            int32_t v = o->x[f];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        o->mn[b] = (int16_t)(lo >> 16);
        o->mx[b] = (int16_t)(hi >> 16);
        o->done++;
        budget = budget > e - a ? budget - (e - a) : 0;
    }
    return o->done == OV_N;
}

int wave_columns(const struct sample *s, int side, int64_t a, int64_t b, int w, int16_t *lo, int16_t *hi)
{
    int64_t span = b - a, fr = s->frames;

    if (span <= 0 || w <= 0 || !fr) {
        return 0;
    }
    if (span <= (int64_t)DIRECT_MAX) {
        for (int c = 0; c < w; c++) {
            int64_t fa = a + span * c / w, fb = a + span * (c + 1) / w;
            int l = wave_at(s, side, fa), h = l;

            for (int64_t f = fa + 1; f < fb; f++) {
                int v = wave_at(s, side, f);
                l = v < l ? v : l;
                h = v > h ? v : h;
            }
            lo[c] = (int16_t)l;
            hi[c] = (int16_t)h;
        }
        return 1;
    }
    struct ov *o = ov_get(s, side);
    if (!ov_build(o)) {
        return 0;
    }
    for (int c = 0; c < w; c++) {
        int64_t fa = a + span * c / w, fb = a + span * (c + 1) / w;
        int l = 0, h = 0, any = 0;

        fa = fa < 0 ? 0 : fa;
        fb = fb > fr ? fr : fb;
        if (fa < fb) {
            int ba = (int)(fa * OV_N / fr), bb = (int)(fb * OV_N / fr);
            bb = bb <= ba ? ba + 1 : bb;
            for (int k = ba; k < bb && k < OV_N; k++) {
                if (!any || o->mn[k] < l) {
                    l = o->mn[k];
                }
                if (!any || o->mx[k] > h) {
                    h = o->mx[k];
                }
                any = 1;
            }
        }
        lo[c] = (int16_t)l;
        hi[c] = (int16_t)h;
    }
    return 1;
}
