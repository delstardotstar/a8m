/* fw/tallent/grains.c against upstream's grain engine (ext/grains.c up to
 * render(), untouched, built beside it by tools/graintest.sh): random
 * settings, CV routes, zones, loops, pitch, FM, rests and retriggers, mono
 * and pairs, forward and reversed. Every output frame must match. Then a
 * ramp sample checks grain starts, spacing and windows on our own. */
#include "grains.h"

#include <stdio.h>
#include <stdlib.h>

void up_start(const struct grain_value *g, const s32 *pcm, u32 first, u32 frames, u32 loop, u32 turn, s32 right,
              int pair);
void up_frame(u32 at, s32 step, s32 *left, s32 *right);
void up_modulate(void);

#define N       200000
#define GUARD   256
#define PLANE   (N + 2 * GUARD)
#define BLOCK   32

static s32 buf[4 * PLANE];
static s32 cv[24];
static u32 rng = 12345;

s32 eq_cv_counts(s32 source, u32 channel)
{
    if (source < -3 || source > 23) return 0;
    return cv[source < 0 ? (u32)(source + 3) * 8 + channel : (u32)source % 3 * 8 + (u32)source / 3];
}

static u32 rnd(u32 n)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return n ? rng % n : rng;
}
static s32 rnd_range(s32 lo, s32 hi) { return lo + (s32)rnd((u32)(hi - lo + 1)); }

static s32 *plane(int k) { return buf + k * PLANE + GUARD; }

static void random_settings(struct grain_value *g)
{
    grain_value_defaults(g);
    g->value[GRAIN_ON] = 1;
    for (int f = GRAIN_SIZE; f < GRAIN_FIELDS; f++)
        if (rnd(3)) g->value[f] = (s16)rnd_range(grain_ranges[f].low, grain_ranges[f].high);
    if (rnd(2)) g->value[GRAIN_SIZE] = (s16)rnd_range(5, 800);
    for (int r = 0; r < GRAIN_ROUTES; r++)
        if (!rnd(3)) {
            g->source[r] = (int8_t)rnd_range(-3, 23);
            g->amount[r] = (int8_t)rnd_range(-100, 100);
        }
}

static int failures;

static void run_case(int n)
{
    struct grain_value g;
    struct grain_tuning t;
    struct grain_state s;
    s32 values[GRAIN_FIELDS];
    int pair = (int)rnd(2), rev = (int)rnd(2);
    u32 first = rnd(N / 2), frames = 2000 + rnd(N - first - 2000);
    u32 loop = 0, turn = frames;

    if (rnd(2)) {
        loop = rnd(frames / 2);
        turn = loop + 4 + rnd(frames - loop - 5);
    }
    for (int i = 0; i < 24; i++) cv[i] = (s32)rnd_range(-32768, 32767) * (s32)rnd(2);
    random_settings(&g);
    /* Upstream reads the reversed buffers (planes 2 and 3) forward. */
    const s32 *up_pcm = plane(rev ? 2 : 0) + first;
    up_start(&g, up_pcm, first, frames, loop, turn, pair ? (s32)PLANE : 0, pair);
    struct grain_zone z = { rev ? plane(0) + N - 1 - first : plane(0) + first, pair ? (s32)PLANE : 0, rev ? -1 : 1,
                            frames, loop, turn };
    grain_fields(&g, 0, values);
    grain_derive(values, &t);
    grain_start(&s, &t, 0);

    s64 pos = (s64)rnd(frames) << 25;
    s32 step = rnd_range(1 << 23, 1 << 27);
    int parked = 0;
    for (int b = 0; b < 1500; b++) {
        u32 at[BLOCK];
        s32 steps[BLOCK], l[BLOCK], r[BLOCK];

        if (!rnd(40)) {
            for (int i = 0; i < 24; i++) cv[i] = (s32)rnd_range(-32768, 32767);
            up_modulate();
            grain_fields(&g, 0, values);
            grain_derive(values, &t);
        }
        if (!rnd(30)) step = rnd(4) ? rnd_range(1 << 22, 1 << 27) : -rnd_range(1 << 22, 1 << 26);
        if (!rnd(60)) parked = !parked;
        if (!rnd(50)) pos = (s64)rnd(frames) << 25;
        int fm = !rnd(5);
        for (int f = 0; f < BLOCK; f++) {
            s32 st = fm ? step + rnd_range(-(1 << 23), 1 << 23) : step;
            pos += st;
            if (pos >= (s64)turn << 25) pos -= (s64)(turn - loop) << 25;
            if (pos < 0) pos = 0;
            at[f] = parked ? GRAIN_PARKED : (u32)(pos >> 25);
            steps[f] = st;
        }
        grain_render(&s, &t, &z, pair, at, steps, l, r, BLOCK);
        for (int f = 0; f < BLOCK; f++) {
            s32 ul, ur;
            up_frame(at[f], steps[f], &ul, &ur);
            if (ul != l[f] || (pair && ur != r[f])) {
                if (failures++ < 10)
                    printf("case %d (pair %d rev %d) block %d frame %d: %d %d vs upstream %d %d\n", n, pair, rev, b, f,
                           l[f], pair ? r[f] : 0, ul, pair ? ur : 0);
                return;
            }
        }
    }
}

/* Ramp (the first grain is due a frame early: its position and phase start a frame on): frame i holds i << 12 (Q31), so a grain's output divided by its
 * window is where it reads. Scan 0 freezes the start at the note's
 * playhead plus Slide; each grain starts there, Interval frames apart. */
static void ramp(void)
{
    struct grain_value g;
    struct grain_tuning t;
    struct grain_state s;
    s32 values[GRAIN_FIELDS], l[BLOCK], r[BLOCK], steps[BLOCK];
    u32 at[BLOCK];
    const u32 head = 50000;

    for (int i = -GUARD; i < N + GUARD; i++) plane(0)[i] = i < 0 || i >= N ? 0 : i << 12;
    grain_value_defaults(&g);
    g.value[GRAIN_ON] = 1;
    g.value[GRAIN_SIZE] = 100;      /* 10 ms: 480 frames */
    g.value[GRAIN_OVERLAP] = 100;   /* one at a time */
    g.value[GRAIN_SPRAY] = 0;
    g.value[GRAIN_SCAN] = 0;
    g.value[GRAIN_SLIDE] = 100;     /* +10 % of the zone */
    g.value[GRAIN_SHAPE] = 50;
    struct grain_zone z = { plane(0), 0, 1, 100000, 0, 100000 };
    grain_fields(&g, 0, values);
    grain_derive(values, &t);
    grain_start(&s, &t, 0);
    u32 expect = head + 10000, frame = 0, starts = 0, bad = 0, last_start = 0;
    s32 prev = 0;
    for (int b = 0; b < 150; b++) {
        for (int f = 0; f < BLOCK; f++) {
            at[f] = head + (u32)(b * BLOCK + f);
            steps[f] = 1 << 25;
        }
        grain_render(&s, &t, &z, 0, at, steps, l, r, BLOCK);
        for (int f = 0; f < BLOCK; f++, frame++) {
            /* A grain's first frame reads its start at the window's floor. */
            if (prev == 0 && l[f] != 0) {
                if (starts && frame - last_start != t.interval >> 8) bad++;
                last_start = frame;
                starts++;
            }
            prev = l[f];
        }
        for (u32 n = 0; n < GRAINS; n++) {
            const struct grain *gr = &s.grains[n];
            if (gr->phase_step) {
                u32 read = (u32)(gr->position >> 32), age = gr->phase / gr->phase_step;
                if (read + 1 != expect + age && read != expect + age && bad++ < 3) printf("read %u expect %u age %u\n", read, expect, age);
            }
        }
    }
    /* The window reaches full level. */
    grain_start(&s, &t, 0);
    u32 peak = 0;
    for (int b = 0; b < 15; b++) {
        for (int f = 0; f < BLOCK; f++) {
            at[f] = head;
            steps[f] = 1 << 25;
        }
        for (int f = 0; f < BLOCK; f++) {
            grain_render(&s, &t, &z, 0, at + f, steps + f, l + f, r + f, 1);
            const struct grain *gr = &s.grains[0];
            if (!gr->phase_step) continue;
            u32 w = (u32)((s64)l[f] * 1024 / ((s64)(expect + gr->phase / gr->phase_step) << 12));
            if (w > peak) peak = w;
        }
    }
    printf("ramp: %u grains, %u frames apart (interval %u), %u mismatches, peak window %u/1024\n", starts,
           t.interval >> 8, t.interval >> 8, bad, peak);
    if (bad || starts < 8 || peak < 1000 || peak > 1030) failures++;
}

int main(void)
{
    for (int k = 0; k < 4; k++)
        for (int i = -GUARD; i < N + GUARD; i++) buf[k * PLANE + GUARD + i] = 0;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < N; i++) plane(k)[i] = (s32)(rnd(0) & 0xfffffff0u) / 2;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < N; i++) plane(k + 2)[i] = plane(k)[N - 1 - i];
    for (int n = 0; n < 300; n++) run_case(n);
    printf("upstream: 300 cases, %d differ\n", failures);
    ramp();
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures != 0;
}
