/* EXTRA=-DINTERP_TEST (profile builds): interp_run / interp_run_f against
 * the plain NEON reference they replaced, on random positions, kernels and
 * samples, bit for bit. Included at the end of interp.c. */

static float ref_kc[ROWS * TAPS] __attribute__((aligned(16)));

static const float *ref_kload(int k)
{
    const int16_t *c = tab + kinfo[k].off;

    for (int i = 0; i < ROWS * kinfo[k].n; i += 4) {
        vst1q_f32(ref_kc + i, vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c + i)), 15));
    }
    return ref_kc;
}

static void ref_dot_f(const float *s, const float *c0, int n, uint32_t fr, float *out)
{
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, b0 = a0, b1 = a0;
    const float *e = s + (n & ~7);

    if (!(fr & WMASK)) {
        for (; s < e; s += 8, c0 += 8) {
            a0 = vmlaq_f32(a0, vld1q_f32(s), vld1q_f32(c0));
            b0 = vmlaq_f32(b0, vld1q_f32(s + 4), vld1q_f32(c0 + 4));
        }
        if (n & 4) {
            a0 = vmlaq_f32(a0, vld1q_f32(s), vld1q_f32(c0));
        }
        a0 = vaddq_f32(a0, b0);
    } else {
        const float *c1 = c0 + n;
        for (; s < e; s += 8, c0 += 8, c1 += 8) {
            float32x4_t v = vld1q_f32(s), w = vld1q_f32(s + 4);
            a0 = vmlaq_f32(a0, v, vld1q_f32(c0));
            a1 = vmlaq_f32(a1, v, vld1q_f32(c1));
            b0 = vmlaq_f32(b0, w, vld1q_f32(c0 + 4));
            b1 = vmlaq_f32(b1, w, vld1q_f32(c1 + 4));
        }
        if (n & 4) {
            float32x4_t v = vld1q_f32(s);
            a0 = vmlaq_f32(a0, v, vld1q_f32(c0));
            a1 = vmlaq_f32(a1, v, vld1q_f32(c1));
        }
        a0 = vaddq_f32(a0, b0);
        a1 = vaddq_f32(a1, b1);
        float32x2_t r = vcvt_n_f32_u32(vdup_n_u32(fr & WMASK), 27);
        a0 = vmlaq_lane_f32(a0, vsubq_f32(a1, a0), r, 0);
    }
    float32x2_t h = vadd_f32(vget_low_f32(a0), vget_high_f32(a0));
    vst1_lane_f32(out, vpadd_f32(h, h), 0);
}

static void ref_dot_q(const float *s, const int16_t *c0, int n, uint32_t fr, float *out)
{
    const int16_t *c1 = c0 + n;
    float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0;

    for (int j = 0; j < n; j += 4) {
        float32x4_t v = vld1q_f32(s + j);
        a0 = vmlaq_f32(a0, v, vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c0 + j)), 15));
        a1 = vmlaq_f32(a1, v, vcvtq_n_f32_s32(vmovl_s16(vld1_s16(c1 + j)), 15));
    }
    float32x2_t r = vcvt_n_f32_u32(vdup_n_u32(fr & WMASK), 27);
    a0 = vmlaq_lane_f32(a0, vsubq_f32(a1, a0), r, 0);
    float32x2_t h = vadd_f32(vget_low_f32(a0), vget_high_f32(a0));
    vst1_lane_f32(out, vpadd_f32(h, h), 0);
}

static void ref_run(const int32_t *x, const int64_t *q, const uint16_t *k, float *out, unsigned n)
{
    int lo = (int)(q[0] >> 32), hi = lo, same = 1;

    for (unsigned f = 1; f < n; f++) {
        int i = (int)(q[f] >> 32);
        lo = i < lo ? i : lo;
        hi = i > hi ? i : hi;
        same &= k[f] == k[0];
    }
    if (hi - lo > INTERP_SPAN_MAX) {
        for (unsigned f = 0; f < n; f++) {
            out[f] = interp_at(x, q[f], k[f]);
        }
        return;
    }
    static float sb[INTERP_SPAN_MAX + TAPS + 4] __attribute__((aligned(16)));
    const int32_t *xs = x + lo - CENTRE;
    const int cnt = hi - lo + TAPS;
    for (int i = 0; i < cnt; i += 4) {
        int j = i < cnt - 4 ? i : cnt - 4;
        vst1q_f32(sb + j, vcvtq_n_f32_s32(vld1q_s32(xs + j), 31));
    }
    if (!same) {
        for (unsigned f = 0; f < n; f++) {
            const struct kinfo *ki = &kinfo[k[f]];
            uint32_t fr = (uint32_t)q[f];
            ref_dot_q(sb + ((int)(q[f] >> 32) - lo) + ki->lo, tab + ki->off + (fr >> 27) * ki->n, ki->n, fr, out + f);
        }
        return;
    }
    const struct kinfo *ki = &kinfo[k[0]];
    const float *kc = ref_kload(k[0]);
    for (unsigned f = 0; f < n; f++) {
        uint32_t fr = (uint32_t)q[f];
        ref_dot_f(sb + ((int)(q[f] >> 32) - lo) + ki->lo, kc + (fr >> 27) * (uint32_t)ki->n, ki->n, fr, out + f);
    }
}

static void ref_run_f(const float *x, const int64_t *q, const uint16_t *k, float *out, unsigned n)
{
    int same = 1;

    for (unsigned f = 1; f < n; f++) {
        same &= k[f] == k[0];
    }
    for (unsigned f = 0; f < n; f++) {
        const struct kinfo *ki = &kinfo[k[f]];
        uint32_t fr = (uint32_t)q[f];
        const float *s = x + (int)(q[f] >> 32) - CENTRE + ki->lo;
        if (same) {
            ref_dot_f(s, ref_kload(k[0]) + (fr >> 27) * (uint32_t)ki->n, ki->n, fr, out + f);
        } else {
            ref_dot_q(s, tab + ki->off + (fr >> 27) * ki->n, ki->n, fr, out + f);
        }
    }
}

static uint32_t it_rng = 12345;

static uint32_t it_rand(void)
{
    it_rng = it_rng * 1664525u + 1013904223u;
    return it_rng ^ (it_rng >> 15);
}

/* Returns the number of outputs that differ (bits) over `rounds` calls. */
unsigned interp_selftest(unsigned rounds, unsigned *calls_direct)
{
    static int32_t plane[8192];
    static float fplane[8192] __attribute__((aligned(16)));
    unsigned bad = 0;

    for (int i = 0; i < 8192; i++) {
        plane[i] = (int32_t)it_rand() >> (it_rand() % 12);
        fplane[i] = (float)(int32_t)it_rand() * (1.0f / 2147483648.0f);
    }
    *calls_direct = 0;
    for (unsigned r = 0; r < rounds; r++) {
        int64_t q[32];
        uint16_t k[32];
        float a[32], b[32];
        unsigned n = 1 + it_rand() % 32, mode = it_rand() % 8;
        int k0 = (int)(it_rand() % INTERP_KERNELS);
        if (mode < 2) {
            k0 = 1024 + (int)(it_rand() % 4000);
        } else if (mode == 2) {
            k0 = 664 + (int)(it_rand() % 400);
        }
        uint64_t step = (uint64_t)it_rand() << (it_rand() % 3);
        if (mode == 3) {
            step = (uint64_t)1 << 32;
        }
        int64_t p = ((int64_t)(200 + it_rand() % 7000) << 32) | (mode == 4 ? 0x80000000u : it_rand());
        for (unsigned f = 0; f < n; f++) {
            p += (int64_t)step;
            q[f] = mode == 5 ? p - (int64_t)(it_rand() % 64) * ((int64_t)1 << 30) : p;
            k[f] = (uint16_t)(mode == 6 ? (int)(it_rand() % INTERP_KERNELS) : mode == 7 ? (k0 + (int)(f / 9)) % INTERP_KERNELS : k0);
        }
        if (it_rand() % 4 == 0) {
            interp_block();
        }
        unsigned room = kload_room;
        int slot = (int)(it_rand() % INTERP_SLOTS);
        int was = kcache_k[slot];
        interp_run(slot, plane, q, k, a, n);
        *calls_direct += was != k[0] && kcache_k[slot] != k[0] && room < ROWS * TAPS;
        ref_run(plane, q, k, b, n);
        for (unsigned f = 0; f < n; f++) {
            union { float f; uint32_t u; } x = { a[f] }, y = { b[f] };
            bad += x.u != y.u;
        }
        for (unsigned f = 0; f < n; f++) {
            q[f] = (q[f] & 0xffffffffll) | ((int64_t)(70 + it_rand() % 8000) << 32);
            k[f] = (uint16_t)(k[f] % (INTERP_SHORT + 1));
        }
        interp_run_f(slot, fplane, q, k, a, n);
        ref_run_f(fplane, q, k, b, n);
        for (unsigned f = 0; f < n; f++) {
            union { float f; uint32_t u; } x = { a[f] }, y = { b[f] };
            bad += x.u != y.u;
        }
    }
    return bad;
}
