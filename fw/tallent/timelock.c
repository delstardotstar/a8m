/* Time Lock: moves the voice's playhead to each hit on the tempo's
 * timeline and fills pitched-up slices. Positions are Q32 sample frames.
 * Local: upstream's ext/timelock.c engine as a library (see UPSTREAM). */

#include "timelock.h"

static u32 sine[SINE_STEPS + 1];     /* Q30 sin(pi/2 i/SINE_STEPS) */
static u32 reciprocal[FADES_MAX + 1]; /* Q16 / n: a fade's phase step */
u32 timelock_seed = RANDOM_SEED; /* local: named, so a test can replay */
struct timelock_stats { u32 joins, skipped, onsets, fills, runouts, resyncs, planned, waits; } timelock_stats;
static u32 streams_live; /* reads fading out across the channels at the frame's start */
u32 timelock_frames_now; /* output frames counted, for the clocks */

/* Left shifts of signed values as the hardware does them (no UB). */
static inline s32 shl(s32 x, u32 n) { return (s32)((u32)x << n); }
static inline s64 q32(s32 step) { return (s64)step * (1ll << STEP_TO_Q32); }

/* ---- interrupt ---- */

/* The high word of a signed product: one smmul. */
static inline s32 high(s32 a, s32 b) { return (s32)(((s64)a * b) >> 32); }

/* Four-point cubic through p[-1..2] at fraction f (Q31), Q27 out, as grain
 * mode reads. */
static inline s32 interpolate(const s32 *p, s32 f)
{
    s32 a = p[-1] >> SAMPLE_SHIFT, b = p[0] >> SAMPLE_SHIFT, c = p[1] >> SAMPLE_SHIFT, d = p[2] >> SAMPLE_SHIFT;
    s32 c1 = (c - a) >> 1;
    s32 c2 = a - 2 * b - (b >> 1) + 2 * c - (d >> 1);
    s32 c3 = ((d - a) >> 1) + (b - c) + ((b - c) >> 1);
    s32 y = shl(high(c3, f), 1);
    y = shl(high(y + c2, f), 1);
    y = shl(high(y + c1, f), 1);
    return y + b;
}

static inline u32 sine_at(u32 phase) /* phase Q16 of a quarter turn */
{
    u32 i = phase >> 8, f = phase & 0xffu;
    if (i >= SINE_STEPS) return sine[SINE_STEPS];
    return sine[i] + (((sine[i + 1] - sine[i]) * f) >> 8); /* local: in 32 bits (a step < 2^23) */
}

/* A crossfade's two gains at its phase, Q30: equal gain (sin^2) blended
 * with equal power (sin) by the law. */
static inline u32 fade_in_gain(const struct fade *f)
{
    u32 s = sine_at(f->phase);
    if (!f->law) return s; /* local: equal power alone, the same value */
    u32 s2 = (u32)(((u64)s * s) >> 30);
    return (u32)(((u64)s2 * f->law + (u64)s * (256u - f->law)) >> 8);
}
static inline u32 fade_out_gain(const struct fade *f)
{
    u32 c = sine_at((1u << 16) - f->phase);
    if (!f->law) return c;
    u32 c2 = (u32)(((u64)c * c) >> 30);
    return (u32)(((u64)c2 * f->law + (u64)c * (256u - f->law)) >> 8);
}

static inline void fade_start(struct fade *f, u32 frames, u32 law)
{
    if (frames > FADES_MAX) frames = FADES_MAX;
    if (!frames) frames = 1;
    f->step = reciprocal[frames]; f->phase = f->step >> 1; f->law = (u8)law;
}
static inline int fade_done(const struct fade *f) { return f->phase >= (1u << 16); }

static inline s32 saturate(s64 x) { return x > Q31_MAX ? Q31_MAX : x < Q31_MIN ? Q31_MIN : (s32)x; }
/* A Q27 read times a Q30 gain, as Q31. */
static inline s32 heard(s32 y, u32 g) { return shl(high(y, (s32)(g >= ONE_Q30 ? Q31_MAX : g << 1)), 1u + SAMPLE_SHIFT); }
/* x (Q31) times a Q30 gain. */
static inline s32 scale30(s32 x, u32 g) { return shl(high(x, (s32)(g >= ONE_Q30 ? Q31_MAX : g << 1)), 1); }

/* The first position index past `frame`. */
u32 timelock_after(const struct lock_zone *z, u32 frame)
{
    u32 lo = 0, hi = z->count;
    while (lo < hi) {
        u32 mid = (lo + hi) >> 1;
        if ((z->positions[mid] & ~HIT_FLAG) <= frame) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static inline int looping(const struct lock_zone *z) { return z->turn < z->end; }
static inline s64 frames_q32(u32 frame) { return (s64)frame << 32; }
static inline s64 round_q32(const struct lock_zone *z) { return (s64)(z->turn - z->loop) << 32; }

/* Where the slice ends: the next hit past the cursor, going round the loop
 * once; with none, the zone's end. */
static void slice_end(struct lock *l, const struct lock_zone *z)
{
    s64 lap = l->a_lap;
    u32 i = l->cursor;
    for (u32 turns = 0; turns < 2; ++turns) {
        for (; i < z->count; ++i) {
            u32 p = z->positions[i], frame = p & ~HIT_FLAG;
            if (frame >= z->turn) break;
            if (p & HIT_FLAG) { l->b = frames_q32(frame) + lap; l->b_lap = lap; l->b_index = i; l->b_hit = 1; return; }
        }
        if (!looping(z)) break;
        i = z->loop_index; lap += round_q32(z);
    }
    l->b = frames_q32(z->end) + l->a_lap; l->b_lap = l->a_lap; l->b_index = z->count; l->b_hit = 0;
}

/* The slice holding the voice's real frame: its start is the last hit at or
 * before it, a lap back round the loop if need be. */
static void slice_start(struct lock *l, const struct lock_zone *z, u32 frame)
{
    u32 floor = frame >= z->loop && looping(z) ? z->loop : z->first, i = timelock_after(z, frame);
    for (u32 k = i; k-- > 0;) {
        u32 p = z->positions[k];
        if ((p & ~HIT_FLAG) < floor) break;
        if (p & HIT_FLAG) { l->a = frames_q32(p & ~HIT_FLAG); l->a_lap = 0; l->cursor = k + 1; return; }
    }
    if (looping(z) && frame >= z->loop)
        for (u32 k = z->count; k-- > z->loop_index;) {
            u32 p = z->positions[k];
            if ((p & ~HIT_FLAG) >= z->turn) continue;
            if (p & HIT_FLAG) { l->a_lap = -round_q32(z); l->a = frames_q32(p & ~HIT_FLAG) + l->a_lap; l->cursor = k + 1; return; }
        }
    l->a = frames_q32(floor); l->a_lap = 0; l->cursor = i;
}

/* The last rise after the slice's start and before `at` (unrolled), or 0. */
static s64 last_rise(const struct lock *l, const struct lock_zone *z, s64 at)
{
    s64 found = 0;
    for (u32 i = l->cursor; i < z->count && i < l->b_index; ++i) {
        u32 frame = z->positions[i] & ~HIT_FLAG;
        if (frame >= z->turn || frames_q32(frame) + l->a_lap >= at) break;
        found = frames_q32(frame) + l->a_lap;
    }
    return found;
}

static inline u32 output_frames(const struct lock_zone *z, s64 frames_q32_) { return (u32)(((u64)(frames_q32_ >> 32) * z->inverse) >> 31); }

static u32 random(void)
{
    timelock_seed ^= timelock_seed << 13; timelock_seed ^= timelock_seed >> 17; timelock_seed ^= timelock_seed << 5;
    return timelock_seed;
}

/* A fill piece's length: within a quarter of `piece` either way. */
static u32 jittered(u32 piece)
{
    u32 jitter = (u32)(((u64)random() * (piece >> (PIECE_JITTER_SHIFT - 1))) >> 32);
    return piece - (piece >> PIECE_JITTER_SHIFT) + jitter;
}

/* A slice starts: decide how it plays at this pitch. */
static void slice_plan(struct lock *l, const struct lock_zone *z, u32 own_q16)
{
    l->length = output_frames(z, l->b - l->a);
    l->own = own_q16 ? (u32)(((u64)l->length * own_q16) >> 16) : ~0u;
    l->runout_phase = 0;
    l->due = DUE_NONE;
    l->onset_at = l->b_hit ? l->b - (s64)PREROLL_FRAMES * q32(z->base) : l->b;
    if (l->own >= l->length) { l->mode = MODE_PLAY_ON; return; } /* pitched down: cut at the next hit */
    u32 attack = ATTACK_FRAMES;
    if (attack > l->length / 2) attack = l->length / 2;
    if (attack > l->own / 2) attack = l->own / 2;
    u32 settle = SETTLE_FRAMES < l->own / 3 ? SETTLE_FRAMES : l->own / 3;
    u32 avail = l->own - settle, rest = l->length - attack;
    u32 x = avail / 4 > FILL_FADE_MIN ? avail / 4 : FILL_FADE_MIN;
    if (x > FILL_FADE_FRAMES) x = FILL_FADE_FRAMES;
    if (x > rest / 3) x = rest / 3;
    if (x < FILL_FADE_FLOOR || avail < 3 * x || rest < 2 * x) {
        l->mode = MODE_RUNOUT;
        l->due = l->own > RUNOUT_FRAMES ? (s32)(l->own - RUNOUT_FRAMES) : 0; /* checked by position from here */
        return;
    }
    u32 piece = PIECE_FRAMES > 2 * x ? PIECE_FRAMES : 2 * x;
    if (piece > avail - x) piece = avail - x;
    l->mode = MODE_FILL;
    l->attack = attack; l->xfade = x; l->piece = piece;
    l->next_piece = jittered(piece);
    l->due = (s32)attack;
}


/* Move the voice (and its Stereo/Right partner) to `to` (unrolled) and fade
 * the read it leaves out over `frames`; the voice fades in. */
static int join(struct lock *l, const struct lock_zone *z, s64 now, s64 to, u32 frames, u32 law, int heard, s64 *write)
{
    s64 gap = to - (now + l->lap);
    if (gap < (s64)SKIP_Q32 && gap > -(s64)SKIP_Q32) { ++timelock_stats.skipped; return 0; }
    if (heard) {
        struct stream *s = &l->streams[0];
        if (s->active) s = &l->streams[1];
        if (s->active) /* both busy: the older gives way to this one */
            s = l->streams[0].out.phase > l->streams[1].out.phase ? &l->streams[0] : &l->streams[1];
        s->position = now;
        fade_start(&s->out, frames, law);
        s->in = l->in; /* the voice's fade-in still running goes with it */
        if (!s->active) ++streams_live;
        s->active = 1;
    }
    fade_start(&l->in, frames, law);
    if (!heard) l->in.law = 0;
    l->mixing = 1;
    /* The real frame, in the voice's lap or the one either side. */
    s64 real = to - l->lap;
    if (looping(z)) {
        if (real >= frames_q32(z->turn)) { real -= round_q32(z); l->lap += round_q32(z); }
        else if (real < frames_q32(z->loop) && l->lap > 0) { real += round_q32(z); l->lap -= round_q32(z); }
    }
    s64 lowest = frames_q32(z->first + READ_MARGIN), highest = frames_q32(z->end - READ_MARGIN);
    if (real < lowest) real = lowest;
    if (real > highest) real = highest;
    *write = real;
    l->expect = real;
    ++timelock_stats.joins;
    return 1;
}

/* The fading reads and the voice's gains, mixed into this frame. */
static __attribute__((noinline)) void mix_frame(struct lock *l, s32 *mix, struct lock_out *o, u32 pair, const struct lock_zone *z, s32 step)
{
    u32 g = ONE_Q30;
    if (!fade_done(&l->in)) { g = fade_in_gain(&l->in); l->in.phase += l->in.step; }
    if (l->mode == MODE_WAIT) g = 0;
    else if (l->runout_phase) {
        g = (u32)(((u64)g * sine_at((1u << 16) - l->runout_phase)) >> 30);
        l->runout_phase += reciprocal[RUNOUT_FRAMES];
        if (l->runout_phase >= (1u << 16)) { l->mode = MODE_WAIT; l->due = 0; }
    }
    s64 left = scale30(*mix, g), right = 0;
    o->gain = g; o->mixed = 1;
    for (u32 n = 0; n < STREAMS; ++n) {
        struct stream *s = &l->streams[n];
        if (!s->active) continue;
        s->position += q32(step);
        if (looping(z) && s->position >= frames_q32(z->turn)) s->position -= round_q32(z);
        u32 frame = (u32)(s->position >> 32);
        if (frame < z->first + READ_MARGIN || frame + READ_MARGIN >= z->end) { s->active = 0; continue; }
        u32 w = fade_out_gain(&s->out);
        if (!fade_done(&s->in)) { w = (u32)(((u64)w * fade_in_gain(&s->in)) >> 30); s->in.phase += s->in.step; }
        s32 f = (s32)((u32)s->position >> 1);
        __builtin_prefetch(z->pcm[0] + frame + CACHE_LINE_BYTES / sizeof(s32));
        left += heard(interpolate(z->pcm[0] + frame, f), w);
        if (pair && z->pcm[1]) {
            __builtin_prefetch(z->pcm[1] + frame + CACHE_LINE_BYTES / sizeof(s32));
            right += heard(interpolate(z->pcm[1] + frame, f), w);
        }
        s->out.phase += s->out.step;
        if (fade_done(&s->out)) s->active = 0;
    }
    *mix = saturate(left);
    o->right = right;
    l->mixing = !fade_done(&l->in) || l->streams[0].active || l->streams[1].active || l->runout_phase || l->mode == MODE_WAIT;
}

/* Keep the voice where it is for the next frame: it waits, silent, for the
 * next hit. */
static int hold(struct lock *l, s64 now, s32 step, s64 *write)
{
    u64 at = (u64)(now - q32(step));
    *write = (s64)at;
    l->expect = (s64)at;
    return 1;
}

static void disengage(struct lock *l) { timelock_disengage(l); }

/* Start following the voice where it is. */
static void engage(struct lock *l, const struct lock_zone *z, u32 zone, s64 at, u32 own_q16)
{
    disengage(l);
    l->engaged = 1; l->zone = (u8)zone;
    l->timeline = l->expect = at;
    slice_start(l, z, (u32)(at >> 32));
    slice_end(l, z);
    slice_plan(l, z, own_q16);
    l->o = (s32)output_frames(z, at - l->a);
    l->in.phase = 1u << 16;
    ++timelock_stats.resyncs;
}

/* The next slice, a fill piece, the run-out, or holding the voice. */
static int event(struct lock *l, const struct lock_zone *z, u32 own_q16, s64 now, s32 step, s64 *write)
{
    s64 at = now + l->lap, per = q32(step);
    if (l->timeline >= l->onset_at) {
        if (!l->b_hit) { /* the zone's end: play out, or stay held when the slice ran out */
            l->onset_at = ONSET_NONE;
            if (l->mode != MODE_WAIT) { l->mode = MODE_END; l->due = DUE_NONE; return 0; }
            return hold(l, now, step, write);
        }
        /* The voice reaches the hit on time: it reads the frames before the
         * hit, which round the loop's end are the loop's last. */
        s64 early = l->b - l->timeline; /* sample frames to the hit at the sample's speed */
        s64 ratio = (s64)(((s64)step * z->inverse) >> 32); /* Q24 step / base */
        s64 to = l->b - (((early >> 16) * ratio) >> 8);
        int heard = l->mode != MODE_WAIT;
        u32 law = l->plan.key == l->serial && l->plan.kind == PLAN_ONSET ? l->plan.law : 0u;
        ++l->serial;
        l->a = l->b; l->a_lap = l->b_lap; l->cursor = l->b_index + 1;
        slice_end(l, z);
        slice_plan(l, z, own_q16);
        l->o = -(s32)output_frames(z, early);
        int moved = join(l, z, now, to, PREROLL_FRAMES, law, heard, write);
        ++timelock_stats.onsets;
        return moved;
    }
    if (l->mode == MODE_WAIT) return hold(l, now, step, write);
    if (l->mode == MODE_RUNOUT) {
        /* The slice's own audio ends: fade it in time. */
        if (!l->runout_phase && step > 0 && at + (s64)RUNOUT_FRAMES * per >= l->b) {
            l->runout_phase = 1; l->mixing = 1; l->due = DUE_NONE; ++timelock_stats.runouts;
        }
        return 0;
    }
    if (l->mode != MODE_FILL || (u32)l->o >= l->length) { l->due = DUE_NONE; return 0; }
    /* A fill piece: back to the timeline, as near as the slice allows. */
    u32 left = l->length - (u32)l->o, piece = l->next_piece;
    int last = left < piece + piece / 2;
    u32 reads = last ? left : piece + l->xfade;      /* output frames the piece reads */
    s64 lo = l->a + (s64)(SETTLE_FRAMES < l->own / 3 ? SETTLE_FRAMES : l->own / 3) * per;
    s64 rise = last_rise(l, z, at);
    if (rise) {
        s64 guard = rise + (s64)PROTECT_FRAMES * q32(z->base);
        if (guard > lo) lo = guard < at ? guard : at;
    }
    s64 hi = l->b - (s64)reads * per, to = l->timeline;
    u32 law = 0;
    if (l->plan.key == l->serial && l->plan.kind == PLAN_FILL) { to += l->plan.offset; law = l->plan.law; ++timelock_stats.planned; }
    ++l->serial;
    if (to < lo) to = lo;
    if (to > hi) to = hi;
    int moved = join(l, z, now, to, l->xfade, law, 1, write);
    ++timelock_stats.fills;
    l->due = last ? DUE_NONE : l->o + (s32)piece;
    l->next_piece = jittered(l->piece);
    return moved;
}

/* ---- clocks ---- */

int timelock_clock_live(const struct clock *k)
{
    return k->period_q8 && k->primed && k->age < CLOCK_LATE_PERIODS * (k->period_q8 >> 8);
}

/* A tick: the period averages the last `window` intervals; two in a row a
 * fifth off start a new average. */
static __attribute__((noinline)) void clock_tick(struct clock *k, u32 ticks)
{
    /* A tick after the clock stopped measures nothing: the gap is no interval. */
    int restart = !k->primed || (k->period_q8 && k->age >= CLOCK_LATE_PERIODS * (k->period_q8 >> 8));
    if (restart) k->count = 0, k->measuring = 0, k->off = 0;
    else {
        /* Two intervals in a row off the same way are a new tempo; one is a late tick. */
        u32 mean = k->period_q8 >> 8, margin = mean / CLOCK_SNAP_FRACTION, interval = k->age / ticks;
        s32 off = !k->period_q8 ? 0 : interval > mean + margin ? 1 : interval < mean - margin ? -1 : 0;
        if (!k->period_q8) k->count = 0, k->measuring = 1;
        /* A new tempo waits a beat of ticks: a few read together would measure too short. */
        else if (off && off == k->off && k->measuring) k->count = 0, k->measuring = 0;
        k->off = (signed char)off;
    }
    /* Each tick's time; ticks read together share it. */
    for (u32 n = 0; n < ticks; ++n) {
        if (!restart || n) k->count = (u8)(k->count < k->window ? k->count + 1 : k->window);
        k->times[k->next] = timelock_frames_now;
        k->next = (u8)((k->next + 1) & (CLOCK_RING - 1));
    }
    u32 last = (k->next - 1u) & (CLOCK_RING - 1);
    if (k->count && (k->measuring || k->count >= k->settle)) {
        u32 first = k->times[(last - k->count) & (CLOCK_RING - 1)];
        u32 period = ((timelock_frames_now - first) << 8) / k->count;
        if (period >= CLOCK_PERIOD_MIN_FRAMES << 8) k->period_q8 = period, k->measuring = 1;
    }
    k->primed = 1; k->seen += ticks; k->age = 0;
    s64 error = (s64)(((u64)k->seen << 32) - k->position);
    if (restart || error > (s64)(1ull << 32) || error < -(s64)(1ull << 32)) { k->position = (u64)k->seen << 32; ++k->epoch; }
    else k->position += (u64)(error >> CLOCK_PHASE_SHIFT);
}

/* A frame of a clock, with the ticks that arrived in it. After a gap the
 * first tick measures nothing: the clock runs at the period it had. */
void timelock_clock_frame(struct clock *k, u32 ticks)
{
    if (++k->age >= CLOCK_GAP_FRAMES) k->primed = 0;
    u64 lead = (u64)(k->seen + CLOCK_LEAD_TICKS) << 32;
    if (k->primed && k->position + k->inverse < lead) k->position += k->inverse;
    if (ticks) clock_tick(k, ticks);
}

/* Advances the timeline: from the clock's position when it runs, else at the speed. */
static inline void timeline_frame(struct lock *l, const struct lock_zone *z, const struct clock_now *k)
{
    if (!k || !z->tick || !k->live) {
        l->anchored = 0;
        l->timeline += q32(z->base);
        return;
    }
    if (!l->anchored || l->epoch != k->epoch) {
        l->anchored = 1; l->epoch = k->epoch; l->position_at = k->position; l->tick_at = l->timeline;
    }
    s64 d = (s64)(k->position - l->position_at), whole = d >> 32;
    if (whole > 0) { l->tick_at += whole * z->tick; l->position_at += (u64)whole << 32; d -= whole << 32; }
    s64 next = l->tick_at + (((d >> 16) * (z->tick >> 8)) >> 8);
    if (next > l->timeline) l->timeline = next;
}

/* Local: upstream's channel_frame for one channel, its playhead and
 * output passed in, the playhead's write passed out. The interrupt's limits
 * across channels (an event a frame, a cycle budget) and the preloads are
 * left out. */
static inline __attribute__((always_inline)) int frame(struct lock *l, const struct lock_zone *z, u32 zone, u32 pair, u32 own_q16,
                   const struct clock_now *k, s64 now, s32 step, s32 *left, struct lock_out *out, u32 live, s64 *to)
{
    out->mixed = 0;
    streams_live = live;
    l->expect += q32(step);
    if (looping(z) && l->expect >= frames_q32(z->turn)) { l->expect -= round_q32(z); l->lap += round_q32(z); } /* the voice comes round */
    s64 off = now - l->expect;
    if (!l->engaged || l->zone != zone || off > (s64)FOLLOW_Q32 || off < -(s64)FOLLOW_Q32) {
        engage(l, z, zone, now, own_q16);
        return 0;
    }
    l->expect = now;
    timeline_frame(l, z, k);
    ++l->o;
    /* This frame was read where the voice was; a write moves the next. */
    if (l->mixing) mix_frame(l, left, out, pair, z, step);
    if (l->timeline >= l->onset_at) return event(l, z, own_q16, now, step, to);
    if (l->o < l->due) return 0;
    /* A fill piece waits a frame while the channels' fades are full. */
    if (l->mode == MODE_FILL && streams_live >= FILL_STREAMS_MAX && l->waited < FILL_WAIT_MAX) { l->due = l->o + 1; ++l->waited; ++timelock_stats.waits; return 0; }
    l->waited = 0;
    return event(l, z, own_q16, now, step, to);
}

int timelock_frame(struct lock *l, const struct lock_zone *z, u32 zone, u32 pair, u32 own_q16,
                   const struct clock_now *k, s64 now, s32 step, s32 *left, struct lock_out *out, u32 live, s64 *to)
{
    return frame(l, z, zone, pair, own_q16, k, now, step, left, out, live, to);
}

/* Local: timelock_frame over frames 0..n - 1 (now[f] INT64_MIN: the voice
 * ended there), steps[f] or a constant step, k[f] or no clock; stops after
 * a frame that moves the voice (*to). Returns the frames done. */
u32 timelock_run(struct lock *l, const struct lock_zone *z, u32 zone, u32 pair, u32 own_q16, const struct clock_now *k,
                 const s64 *now, const s32 *steps, s32 step, s32 *left, struct lock_out *out, u32 live, u32 n,
                 s64 *to, int *moved)
{
    u32 f = 0;
    *moved = 0;
    /* Quiet frames (no fade, no clock, no event within reach) while the
     * voice follows: frame()'s work without its branches. */
    if (n > 1 && l->engaged && l->zone == zone && !l->mixing && (!k || !z->tick) && l->mode != MODE_WAIT &&
        l->o + (s32)(n - 1) < l->due && l->timeline + (s64)(n - 1) * q32(z->base) < l->onset_at) {
        s64 e = l->expect, lap = l->lap, per = q32(z->base);
        for (; f + 1 < n; ++f) {
            s64 t = now[f], ne = e + q32(steps ? steps[f] : step), nl = lap;
            if (t == (s64)0x8000000000000000ll) break;
            if (looping(z) && ne >= frames_q32(z->turn)) { ne -= round_q32(z); nl += round_q32(z); }
            if (t - ne > (s64)FOLLOW_Q32 || t - ne < -(s64)FOLLOW_Q32) break;
            e = t; lap = nl;
        }
        l->expect = e; l->lap = lap; l->anchored = 0;
        l->timeline += (s64)f * per; l->o += (s32)f;
        for (u32 j = 0; j < f; ++j) out[j].mixed = 0;
    }
    for (; f < n; ++f) {
        if (now[f] == (s64)0x8000000000000000ll) { *moved = -1; return f; }
        if (frame(l, z, zone, pair, own_q16, k ? k + f : 0, now[f], steps ? steps[f] : step, left + f, out + f, live, to)) {
            *moved = 1;
            return f + 1;
        }
    }
    return n;
}

/* Local: when the next frames may first call event(), as timelock_frame
 * would see them with the voice following. */
u32 timelock_quiet(const struct lock *l, const struct lock_zone *z, const struct clock_now *k, u32 n, int *standing)
{
    *standing = 0;
    if (!l->engaged) return 1;
    if (l->runout_phase && l->mode != MODE_WAIT) {
        /* the run-out ends in its frame's mix: a wait, held from then */
        u32 step = reciprocal[RUNOUT_FRAMES], r = ((1u << 16) - l->runout_phase + step - 1) / step;
        n = r < n ? (r ? r : 1) : n;
    }
    if (l->mode != MODE_WAIT && l->o + (s32)n < l->due && (!k || !z->tick || !k->live) &&
        l->timeline + (s64)n * q32(z->base) < l->onset_at)
        return n; /* no event within reach (the clock's timeline can jump: per frame) */
    struct lock c;
    c.timeline = l->timeline; c.anchored = l->anchored; c.epoch = l->epoch;
    c.position_at = l->position_at; c.tick_at = l->tick_at;
    s32 o = l->o;
    for (u32 j = 1; j <= n; ++j) {
        timeline_frame(&c, z, k ? k + j - 1 : 0);
        ++o;
        if (c.timeline >= l->onset_at || o >= l->due) {
            if (l->mode == MODE_WAIT && c.timeline < l->onset_at && j == 1) {
                /* held: every frame until the onset reads where the voice stands */
                u32 m = 1;
                for (; m < n; ++m) {
                    timeline_frame(&c, z, k ? k + m : 0);
                    if (c.timeline >= l->onset_at) break;
                }
                *standing = 1;
                return m;
            }
            return j;
        }
    }
    return n;
}

/* Local: the voice read at `now` with `step` is where the lock expects it. */
int timelock_follows(const struct lock *l, const struct lock_zone *z, u32 zone, s64 now, s32 step)
{
    if (!l->engaged || l->zone != zone) return 0;
    s64 e = l->expect + q32(step);
    if (looping(z) && e >= frames_q32(z->turn)) e -= round_q32(z);
    s64 off = now - e;
    return off <= (s64)FOLLOW_Q32 && off >= -(s64)FOLLOW_Q32;
}

/* Local: a CV clock's rising edge from its counts, as upstream's interrupt. */
u32 timelock_clock_edge(struct clock *k, s32 v)
{
    u32 rise = !k->high && v > CLOCK_HIGH;
    if (rise) k->high = 1; else if (k->high && v < CLOCK_LOW) k->high = 0;
    return rise;
}

void timelock_clock_now(const struct clock *k, struct clock_now *out)
{
    out->position = k->position; out->epoch = k->epoch; out->live = (u8)timelock_clock_live(k);
}

/* ---- foreground ---- */

static int digit(char c) { return c >= '0' && c <= '9'; }
static int letter(char c, char lower) { return (c | ASCII_LOWER) == lower; }

/* Auto Beats: a 60-200 BPM tempo in the sample's name, else the power of two
 * that puts the loop at GUESS_BPM_LOW or above. */
u32 timelock_guess(const char *name, u32 name_bytes, u32 span, u32 rate)
{
    u32 found = 0;
    for (u32 i = 0; i < name_bytes && name[i];) {
        if (!digit(name[i])) { ++i; continue; }
        u32 start = i, value = 0;
        while (i < name_bytes && digit(name[i])) value = value * 10 + (u32)(name[i++] - '0');
        if (i - start > NAME_TEMPO_DIGITS || value < NAME_BPM_LOW || value > NAME_BPM_HIGH) continue;
        int marked = i + 3 <= name_bytes && letter(name[i], 'b') && letter(name[i + 1], 'p') && letter(name[i + 2], 'm');
        if (!found || marked) found = value;
        if (marked) break;
    }
    if (!span || !rate) return 1;
    if (found) {
        u32 beats = (u32)(((u64)found * span * 2 / ((u64)SECONDS_PER_MINUTE * rate) + 1) / 2);
        if (beats && beats <= TIME_LOCK_BEATS_MAX) return beats;
    }
    u32 beats = 1;
    while (beats < TIME_LOCK_BEATS_MAX && (u64)SECONDS_PER_MINUTE * beats * rate < (u64)GUESS_BPM_LOW * span) beats <<= 1;
    return beats;
}

void timelock_init(void)
{
    /* sin(pi/2 i/256) by rotation in Q30: cos and sin of the step. */
    s64 c = 1073721611, s = 6588356, x = ONE_Q30, y = 0;
    for (u32 i = 0; i <= SINE_STEPS; ++i) {
        sine[i] = (u32)(y < 0 ? 0 : y > ONE_Q30 ? ONE_Q30 : y);
        s64 nx = (x * c - y * s) >> 30, ny = (x * s + y * c) >> 30;
        x = nx; y = ny;
    }
    sine[SINE_STEPS] = ONE_Q30;
    for (u32 n = 1; n <= FADES_MAX; ++n) reciprocal[n] = (1u << 16) / n;
    reciprocal[0] = 1u << 16;
}
