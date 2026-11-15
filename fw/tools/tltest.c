/* fw/tallent/timelock.c and joins.c against upstream's Time Lock engine
 * (ext/timelock.c's interrupt half and ext/joins.c, untouched, built beside
 * them by tools/tltest.sh) on one channel, mono and pairs: hit maps from
 * hits.c on synthetic loops, zones with and without loops, Own and Speed
 * tempos, CV and MIDI clocks, pitch, FM, rests and retriggers. Every output
 * frame and every playhead write must match. Then tempo checks of our own:
 * a loop of known BPM locked to a set tempo or a CV clock comes round in
 * the expected time with its hits on the expected frames. */
#include "timelock.h"

#include <stdio.h>
#include <stdlib.h>

void up_start(const void *zone, size_t bytes, int pair, int clock, int input);
void up_clock_setup(int n, u8 window, u8 settle, u32 unused);
u32 up_clock_period(int n);
void up_own(u32 own);
void up_tick(s64 tick, s32 base, u32 inverse);
s64 up_frame(int playing, s64 pos, s32 step, s32 *left, s32 *right);
void up_plan(s32 step);
size_t up_zone_bytes(void);
size_t up_lock_bytes(void);
extern s16 shim_cv[24];
extern u32 patch_midi_clock_ticks;

#define N       400000
#define GUARD   64
#define PLANE   (N + 2 * GUARD)
#define MAP_MAX 512

static s32 buf[2 * PLANE];
static u32 rng = 12345;
static u32 positions[MAP_MAX], count, analysed;
static int failures;
extern u32 timelock_seed;
#define FRAMES 200000
/* Pass 1's frames, replayed through timelock_run in runs (pass 2). */
static struct rec {
    s64 read, up_after;
    s32 step, ul, ur, v;
    u8 playing, midi;
} recs[FRAMES];
static struct lock_zone zones_at[FRAMES / 256 + 1];
static u32 own_at[FRAMES / 64 + 1];
static s32 plan_step[FRAMES / 16 + 1];
extern struct timelock_stats { u32 joins, skipped, onsets, fills, runouts, resyncs, planned, waits; } timelock_stats;

static u32 rnd(u32 n)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return n ? rng % n : rng;
}
static s32 rnd_range(s32 lo, s32 hi) { return lo + (s32)rnd((u32)(hi - lo + 1)); }
static s32 *plane(int k) { return buf + k * PLANE + GUARD; }

/* A drum loop: a hit every `beat` frames (some skipped, some doubled), a
 * thump and a noise burst each, decaying. */
static void make_loop(u32 frames, u32 beat, u32 seed)
{
    u32 keep = rng;
    rng = seed;
    memset(buf, 0, sizeof(buf));
    double env = 0, phase = 0, noise = 0;
    for (u32 i = 0; i < frames; i++) {
        if (i % beat == 0 && rnd(5)) { env = 0.4 + 0.5 * (rnd(100) / 100.0); noise = rnd(2) ? 0.6 : 0.1; phase = 0; }
        if (i % beat == beat / 2 && !rnd(3)) { env = 0.3; noise = 0.8; }
        phase += 0.012;
        double s = phase - (int)phase - 0.5;
        double x = env * ((1 - noise) * s + noise * ((double)rnd(65536) / 32768.0 - 1.0));
        env *= 0.9993;
        plane(0)[i] = (s32)(x * 2147483647.0 * 0.8);
        plane(1)[i] = (s32)(x * 2147483647.0 * 0.7) + (s32)rnd(1 << 20);
    }
    rng = keep;
}

/* The slice map as tlock.c builds it: hits, a hit within 30 ms of the last
 * kept one demoted to a rise. */
static void map_loop(u32 frames, u32 rate, int stereo)
{
    hits_begin(plane(0), stereo ? plane(1) : 0, frames, rate);
    while (!hits_step()) {
    }
    count = hits_take(positions, MAP_MAX, &analysed);
    u32 kept = 0, gap = rate * 30 / 1000;
    int any = 0;
    for (u32 i = 0; i < count; i++) {
        if (!(positions[i] & HIT_FLAG)) continue;
        u32 at = positions[i] & ~HIT_FLAG;
        if (any && at - kept < gap) positions[i] = at;
        else { kept = at; any = 1; }
    }
}

static void zone_setup(struct lock_zone *z, u32 first, u32 end, u32 loop, u32 turn, int pair, s32 base)
{
    memset(z, 0, sizeof(*z));
    z->pcm[0] = plane(0);
    z->pcm[1] = pair ? plane(1) : 0;
    z->positions = positions;
    z->count = count;
    z->first = first;
    z->end = end < analysed ? end : analysed;
    z->loop = loop;
    z->turn = turn ? turn : z->end;
    z->base = base;
    z->inverse = (u32)((1ull << 56) / (u32)base);
    z->loop_index = (u16)(loop ? timelock_after(z, loop - 1u) : 0u);
}

static s32 voice_out(int k, s64 pos, u32 end)
{
    s64 f = pos >> 32;
    return f < 0 || f >= end ? 0 : plane(k)[f];
}

enum { T_OWN, T_SPEED, T_CV, T_MIDI };

/* Pass 2: the same frames through timelock_run in runs of 1-32 frames
 * (cut at the foreground's points), as engine.c drives it: every frame and
 * move must again match upstream's. */
static void replay(int n, int pair, int tempo, u32 end, u32 ppqn, u32 seed0)
{
    struct lock l;
    struct clock ks[2];
    struct lock_zone z = zones_at[0];
    static struct clock_now at[FRAMES];
    u32 clocked = tempo >= T_CV, clock_done = 0;

    memset(&l, 0, sizeof(l));
    memset(ks, 0, sizeof(ks));
    timelock_seed = seed0;
    for (u32 f = 0; f < FRAMES;) {
        if (f % 256 == 0) {
            for (int c = 0; c < 2; c++) {
                u32 p = c ? ppqn : MIDI_PPQN, w = p * CLOCK_BEATS;
                ks[c].window = (u8)(w < CLOCK_RING ? w : CLOCK_RING - 1);
                ks[c].settle = (u8)(p < CLOCK_RING ? p : CLOCK_RING - 1);
                ks[c].inverse = ks[c].period_q8 ? (u32)((1ull << 40) / ks[c].period_q8) : 0;
            }
            z = zones_at[f / 256];
        }
        if (f % 16 == 0) joins_plan(&l, &z, (u32)pair, plan_step[f / 16]);
        u32 m = 1 + rnd(32), stop = (f / 16 + 1) * 16;
        m = f + m > stop ? stop - f : m;
        /* the clocks run ahead of the voice, a block at a time */
        for (; clock_done < f + m; clock_done++) {
            const struct rec *r = &recs[clock_done];
            timelock_frames_now++;
            timelock_clock_frame(&ks[0], r->midi);
            if (tempo == T_CV) timelock_clock_frame(&ks[1], timelock_clock_edge(&ks[1], (s16)r->v));
            timelock_clock_now(&ks[tempo == T_CV ? 1 : 0], &at[clock_done]);
        }
        s64 now[32];
        s32 steps[32], x[32];
        struct lock_out o[32];
        u32 k = 0;
        for (; k < m && recs[f + k].playing; k++) {
            now[k] = recs[f + k].read;
            steps[k] = recs[f + k].step;
            x[k] = voice_out(0, now[k], end);
        }
        if (!k) {
            if (l.engaged) timelock_disengage(&l);
            f++;
            continue;
        }
        if (k < m) now[k++] = (s64)0x8000000000000000ll;
        s64 to;
        int moved;
        u32 streams = l.mixing ? l.streams[0].active + l.streams[1].active : 0;
        u32 done = timelock_run(&l, &z, 0, (u32)pair, own_at[f / 64], clocked ? at + f : 0, now, steps, 0, x, o, streams,
                                k, &to, &moved);
        for (u32 j = 0; j < done; j++) {
            const struct rec *r = &recs[f + j];
            s32 right = voice_out(1, now[j], end);
            right = pair && o[j].mixed ? timelock_right(right, &o[j]) : right;
            s64 after = moved > 0 && j + 1 == done ? to : now[j];
            if (x[j] != r->ul || (pair && right != r->ur) || after != r->up_after) {
                if (failures++ < 10)
                    printf("case %d run at frame %u: %d %d @%lld vs upstream %d %d @%lld\n", n, f + j, x[j], right,
                           (long long)after, r->ul, r->ur, (long long)r->up_after);
                return;
            }
        }
        if (moved < 0) {
            timelock_disengage(&l);
            done++;
        }
        f += done;
    }
}

static void run_case(int n)
{
    static const u8 ppqns[] = { 1, 2, 4, 8, 24 };
    u32 rate = rnd(2) ? 44100 : 48000, frames = 60000 + rnd(N - 60000), beat = 3000 + rnd(12000);
    int pair = (int)rnd(2), tempo = (int)rnd(4);
    make_loop(frames, beat, 1000u + (u32)n);
    map_loop(frames, rate, pair);
    if (!count) return;
    u32 first = rnd(frames / 4), end = first + 20000 + rnd(frames - first - 20000), loop = 0, turn = 0;
    end = end < analysed ? end : analysed;
    if (end <= first + 1000) return;
    if (rnd(3)) {
        loop = first + rnd((end - first) / 2);
        turn = loop + 500 + rnd(end - loop - 500);
    }
    s32 own = (s32)(((u64)rate << AUDIO_STEP_SHIFT) / OUTPUT_HZ), base = own;
    if (tempo == T_SPEED) base = (s32)((s64)own * rnd_range(250, 4000) / 1000);
    u32 ppqn = tempo == T_MIDI ? MIDI_PPQN : ppqns[rnd(5)], beats = 1 + rnd(16), input = rnd(24);
    u32 span = loop ? turn - loop : end - first;
    s64 tick = tempo >= T_CV ? (s64)(((u64)span << 32) / ((u64)beats * ppqn)) : 0;
    struct lock_zone z;
    zone_setup(&z, first, end, loop, turn, pair, base);
    z.tick = tick;
    patch_midi_clock_ticks = 0;
    up_start(&z, sizeof(z), pair, tempo == T_CV ? 2 : tempo == T_MIDI ? 1 : 0, (int)input);

    struct lock l;
    struct clock ks[2];
    memset(&l, 0, sizeof(l));
    memset(ks, 0, sizeof(ks));
    u32 pulse = 200 + rnd(3000), pulse_at = rnd(pulse);
    s64 pos = ((s64)first << 32) - (1ll << 31);
    s32 step = rnd_range(1 << 23, 1 << 27);
    int playing = 1;
    u32 held_own = 0, seed0 = timelock_seed;
    for (u32 f = 0; f < FRAMES; f++) {
        if (f % 256 == 0) {
            /* the foreground: clock windows, the tempo's base, own */
            for (int c = 0; c < 2; c++) {
                u32 p = c ? ppqn : MIDI_PPQN, w = p * CLOCK_BEATS;
                ks[c].window = (u8)(w < CLOCK_RING ? w : CLOCK_RING - 1);
                ks[c].settle = (u8)(p < CLOCK_RING ? p : CLOCK_RING - 1);
                ks[c].inverse = ks[c].period_q8 ? (u32)((1ull << 40) / ks[c].period_q8) : 0;
                up_clock_setup(c, ks[c].window, ks[c].settle, 0);
                if (up_clock_period(c) != ks[c].period_q8) {
                    if (failures++ < 10) printf("case %d frame %u: clock %d period %u vs %u\n", n, f, c, ks[c].period_q8, up_clock_period(c));
                    return;
                }
            }
            const struct clock *k = &ks[tempo == T_CV ? 1 : 0];
            if (tempo >= T_CV && k->period_q8) {
                u64 b = ((u64)span << (AUDIO_STEP_SHIFT + 8)) / ((u64)beats * ppqn * k->period_q8);
                b = b > (u64)own << SPEED_RANGE_SHIFT ? (u64)own << SPEED_RANGE_SHIFT : b;
                b = b < ((u64)own >> SPEED_RANGE_SHIFT) + 1 ? ((u64)own >> SPEED_RANGE_SHIFT) + 1 : b;
                z.base = (s32)b;
                z.inverse = (u32)((1ull << 56) / (u32)z.base);
                up_tick(tick, z.base, z.inverse);
            }
            zones_at[f / 256] = z;
        }
        if (f % 64 == 0) {
            held_own = step > 0 ? (u32)(((u64)z.base << 16) / (u32)step) : 0;
            up_own(held_own);
            own_at[f / 64] = held_own;
            if (!rnd(8)) step = rnd(10) ? rnd_range(1 << 23, 1 << 27) : -rnd_range(1 << 22, 1 << 25);
            if (!rnd(40)) playing = !playing;
            if (!rnd(30)) { pos = ((s64)(first + rnd(end - first - 10)) << 32) - (1ll << 31); playing = 1; }
            if (!rnd(200)) pulse = 200 + rnd(3000);
        }
        if (f % 16 == 0) {
            plan_step[f / 16] = step;
            up_plan(step);
            joins_plan(&l, &z, (u32)pair, step);
        }
        s32 st = (f / 512) % 5 == 1 ? step + rnd_range(-(1 << 22), 1 << 22) : step;
        /* the clocks */
        timelock_frames_now++;
        u32 midi = tempo == T_MIDI && f % pulse == pulse_at;
        patch_midi_clock_ticks += midi;
        timelock_clock_frame(&ks[0], midi);
        s16 v = (s16)(f % pulse < pulse / 2 ? 16384 : 0);
        if (tempo == T_CV) {
            shim_cv[input] = v;
            timelock_clock_frame(&ks[1], timelock_clock_edge(&ks[1], v));
        }
        struct clock_now now;
        timelock_clock_now(&ks[tempo == T_CV ? 1 : 0], &now);
        /* the voice: advance, read */
        pos += (s64)st * 128;
        if (loop && pos >= (s64)turn << 32) pos -= (s64)(turn - loop) << 32;
        if (pos < 0) pos = 0;
        if (!loop && pos >= (s64)end << 32) playing = 0;
        s32 ul = voice_out(0, pos, end), ur = voice_out(1, pos, end), ml = ul, mr = ur;
        recs[f] = (struct rec){ pos, 0, st, 0, 0, v, (u8)playing, (u8)midi };
        s64 up_pos = up_frame(playing, pos, st, &ul, &ur);
        recs[f].ul = ul; recs[f].ur = ur; recs[f].up_after = up_pos;
        s64 my_pos = pos;
        if (!playing) {
            if (l.engaged) timelock_disengage(&l);
        } else {
            struct lock_out o;
            s64 to;
            u32 streams = l.mixing ? l.streams[0].active + l.streams[1].active : 0;
            /* timelock_quiet's promise: no move before the frame it names */
            int standing = 0;
            u32 quiet = tempo >= T_CV ? 1 : timelock_quiet(&l, &z, 0, 64, &standing);
            int follows = timelock_follows(&l, &z, 0, pos, st);
            if (timelock_frame(&l, &z, 0, (u32)pair, held_own, tempo >= T_CV ? &now : 0, pos, st, &ml, &o, streams, &to)) {
                my_pos = to;
                if (quiet > 1 && !standing && follows) {
                    if (failures++ < 10) printf("case %d frame %u: moved %u frames early (mode %d o %d due %d tl %lld onset %lld eng %d)\n", n, f, quiet - 1, l.mode, l.o, l.due, (long long)l.timeline, (long long)l.onset_at, l.engaged);
                    return;
                }
            }
            if (pair) mr = o.mixed ? timelock_right(mr, &o) : mr;
        }
        if (ml != ul || (pair && mr != ur) || my_pos != up_pos) {
            if (failures++ < 10)
                printf("case %d (pair %d tempo %d) frame %u: %d %d @%lld vs upstream %d %d @%lld\n", n, pair, tempo, f, ml,
                       mr, (long long)my_pos, ul, ur, (long long)up_pos);
            return;
        }
        pos = my_pos;
    }
    replay(n, pair, tempo, end, ppqn, seed0);
}

/* A loop of `bpm` and `beats` beats, played at a pitch, locked at Speed
 * (or to a 4 PPQN CV clock at clock_bpm): each hit the voice reaches after
 * an onset comes at the output frame the tempo puts it on, lap after lap. */
static void tempo_check(u32 rate, u32 bpm, u32 beats, s32 speed_tenths, u32 clock_bpm, double pitch)
{
    u32 beat = rate * 60 / bpm, span = beat * beats, ppqn = 4;
    make_loop(span + 4000, beat, 77);
    map_loop(span + 4000, rate, 0);
    s32 own = (s32)(((u64)rate << AUDIO_STEP_SHIFT) / OUTPUT_HZ), step = (s32)(own * pitch);
    s32 base = (s32)((s64)own * (TIME_LOCK_SPEED_UNITY + speed_tenths) / TIME_LOCK_SPEED_UNITY);
    struct lock_zone z;
    zone_setup(&z, 0, span + 2000, 0, span, 0, base);
    if (clock_bpm) z.tick = (s64)(((u64)span << 32) / ((u64)beats * ppqn));
    struct lock l;
    struct clock k;
    memset(&l, 0, sizeof(l));
    memset(&k, 0, sizeof(k));
    k.window = (u8)(ppqn * CLOCK_BEATS);
    k.settle = (u8)ppqn;
    double pulse = clock_bpm ? OUTPUT_HZ * 60.0 / (clock_bpm * ppqn) : 0;
    double target = clock_bpm ? clock_bpm : (double)bpm * (TIME_LOCK_SPEED_UNITY + speed_tenths) / TIME_LOCK_SPEED_UNITY;
    /* sample frames of the timeline an output frame: the speed's, or a
     * clock tick's span over its pulse */
    double per_frame = clock_bpm ? (double)span / ((double)beats * ppqn * pulse)
                                 : (double)rate / OUTPUT_HZ * (TIME_LOCK_SPEED_UNITY + speed_tenths) / TIME_LOCK_SPEED_UNITY;
    u32 lap_frames = (u32)(span / per_frame + 0.5);
    s64 pos = -(1ll << 31), pending = -1;
    u32 onsets = 0, measured = 0, next_tick = 0, ticks = 0;
    double worst = 0, origin = -1e9;
    for (u32 f = 0; f < lap_frames * 4; f++) {
        struct clock_now now;
        if (clock_bpm) {
            if (f % 256 == 0 && k.period_q8) {
                k.inverse = (u32)((1ull << 40) / k.period_q8);
                z.base = (s32)(((u64)span << (AUDIO_STEP_SHIFT + 8)) / ((u64)beats * ppqn * k.period_q8));
                z.inverse = (u32)((1ull << 56) / (u32)z.base);
            }
            u32 tick = f == next_tick;
            if (tick) next_tick = (u32)(++ticks * pulse + 0.5);
            timelock_frames_now++;
            timelock_clock_frame(&k, tick);
            timelock_clock_now(&k, &now);
        }
        pos += (s64)step * 128;
        if (pos >= (s64)span << 32) pos -= (s64)span << 32;
        struct lock_out o;
        s64 to;
        s32 x = plane(0)[pos >> 32];
        u32 own_q16 = (u32)(((u64)z.base << 16) / (u32)step), serial = l.serial;
        int moved = timelock_frame(&l, &z, 0, 0, own_q16, clock_bpm ? &now : 0, pos, step, &x, &o, 0, &to);
        if (moved) pos = to;
        if (l.serial != serial && l.o < 0) { pending = l.a; onsets++; }
        if (pending >= 0 && pos + l.lap >= pending) {
            /* the hit's frame on the tempo's grid, the grid's phase from the
             * first hit measured (clocks: once settled, after two laps) */
            double at = f - (double)(pending >> 32) / per_frame;
            if (f > lap_frames * 2 || !clock_bpm) {
                if (origin < -1e8) origin = at;
                double err = at - origin;
                if (getenv("TLDBG")) printf("  f %u hit %lld err %.1f\n", f, (long long)(pending >> 32), err);
                err = err < 0 ? -err : err;
                measured++;
                if (err > worst) worst = err;
            }
            pending = -1;
        }
    }
    int ok = measured > 4 && worst <= (clock_bpm ? 8 : 2);
    printf("tempo: %u BPM x%u beats at %u Hz, %s %.1f BPM, pitch x%.2f: %u onsets, %u hits off the grid by <= %.1f frames %s\n",
           bpm, beats, rate, clock_bpm ? "CV clock" : "speed", target, pitch, onsets, measured, worst, ok ? "ok" : "BAD");
    failures += !ok;
}

int main(void)
{
    timelock_init();
    if (up_zone_bytes() != sizeof(struct lock_zone) || up_lock_bytes() != sizeof(struct lock)) {
        printf("layouts differ\n");
        return 1;
    }
    for (int n = 0; n < 200; n++) run_case(n);
    printf("upstream: 200 cases, %d differ (joins %u, onsets %u, fills %u, runouts %u, planned fills %u, resyncs %u)\n",
           failures, timelock_stats.joins, timelock_stats.onsets, timelock_stats.fills, timelock_stats.runouts,
           timelock_stats.planned, timelock_stats.resyncs);
    tempo_check(44100, 120, 8, 0, 0, 1.0);
    tempo_check(44100, 120, 8, 0, 0, 1.5);
    tempo_check(44100, 120, 8, 0, 0, 0.6);
    tempo_check(48000, 170, 16, -250, 0, 1.26);
    tempo_check(44100, 95, 4, 600, 0, 0.8);
    tempo_check(44100, 120, 8, 0, 140, 1.0);
    tempo_check(48000, 100, 8, 0, 90, 1.33);
    printf(failures ? "FAIL\n" : "PASS\n");
    return failures != 0;
}
