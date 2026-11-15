#include "tlock.h"
#include "audio.h"
#include "cv.h"
#include "engine.h"
#include "eq.h"
#include "fx.h"
#include "mix_fx.h"
#include "preset.h"
#include "samples.h"
#include "systime.h"

#include <string.h>

/* Slice maps (upstream ext/slice_maps.c, ours): each locked sample's hits,
 * kept while a zone uses it, found a pass at a time in the main loop. */
#define SLICE_MAPS              (CHANNELS * ZONES)
#define SLICE_MAP_POSITIONS     512u    /* about 25 s of a busy loop; past that, one slice */
#define ANALYSIS_PASS_US        1000u   /* of each main loop pass (upstream 2 ms) */
#define SLICE_MIN_MS            30u     /* a hit this close after the last kept one starts no slice */

enum { MAP_FREE, MAP_WAITING, MAP_READY };

struct slice_map {
    int smp;
    const int32_t *frame0;
    uint32_t frames, analysed, count;
    uint8_t state, used;
};

static struct slice_map maps[SLICE_MAPS];
static uint32_t map_positions[SLICE_MAPS][SLICE_MAP_POSITIONS];
static int analysing = -1;

/* The tables the interrupt plays from (upstream's struct lock_table). */
struct tl_tables {
    struct lock_zone zone[CHANNELS][ZONES];
    uint32_t analysed[CHANNELS][ZONES];
    uint8_t on[CHANNELS], pair[CHANNELS];
    uint8_t clock[CHANNELS];            /* clocks index + 1; 0: none */
    uint8_t clock_input[CLOCKS];        /* cv_frames input, or CLOCK_INPUT_NONE */
};
static struct tl_tables tables[2];
static volatile uint32_t live;
static uint8_t polled;

static struct lock locks[CHANNELS];
static struct clock clocks[CLOCKS];
static struct clock_now clock_at[CLOCKS][AUDIO_BLOCK];
/* The delay's CV CLOCK (mix_fx.h), run beside the channels' clocks. */
static struct clock delay_clock;
static volatile uint8_t delay_input = CLOCK_INPUT_NONE;
static uint32_t delay_ppqn;
static struct lock_zone work[CHANNELS];
static volatile int32_t steps_now[CHANNELS];
static uint32_t streams_now;

/* ---- slice maps ---- */

static const struct sample *ready(int smp)
{
    return samples_valid(smp) && samples[smp].state == SMP_READY && samples[smp].plane[0] ? &samples[smp] : 0;
}

static void claim(int smp)
{
    const struct sample *sm = &samples[smp];
    int free = -1;

    for (int m = 0; m < SLICE_MAPS; m++) {
        struct slice_map *map = &maps[m];
        if (map->state == MAP_FREE) {
            free = free < 0 ? m : free;
            continue;
        }
        if (map->smp == smp && map->frame0 == sm->plane[0] && map->frames == sm->frames) {
            map->used = 1;
            return;
        }
    }
    if (free < 0) {
        return;
    }
    struct slice_map *map = &maps[free];
    map->smp = smp;
    map->frame0 = sm->plane[0];
    map->frames = sm->frames;
    map->analysed = map->count = 0;
    map->used = 1;
    __asm__ volatile("dmb" ::: "memory");
    map->state = MAP_WAITING;
}

static void start(int m)
{
    const struct sample *sm = &samples[maps[m].smp];

    hits_begin(maps[m].frame0, sm->channels == 2 ? sm->plane[1] : 0, maps[m].frames, sm->rate);
    analysing = m;
}

static void finish(int m)
{
    struct slice_map *map = &maps[m];
    uint32_t *p = map_positions[m], kept = 0, gap = samples[map->smp].rate * SLICE_MIN_MS / 1000u;
    int any = 0;

    map->count = hits_take(p, SLICE_MAP_POSITIONS, &map->analysed);
    /* A hit too soon after the one before stays a rise. */
    for (uint32_t i = 0; i < map->count; i++) {
        if (!(p[i] & HIT_FLAG)) {
            continue;
        }
        uint32_t at = p[i] & ~HIT_FLAG;
        if (any && at - kept < gap) {
            p[i] = at;
        } else {
            kept = at;
            any = 1;
        }
    }
    __asm__ volatile("dmb" ::: "memory");
    map->state = MAP_READY;
    analysing = -1;
}

static int wanted(int ch)
{
    const struct channel_cfg *c = &preset.ch[preset_param_channel(ch)];

    return c->tl.on && !c->machine;
}

static void maps_poll(void)
{
    for (int m = 0; m < SLICE_MAPS; m++) {
        maps[m].used = 0;
    }
    for (int c = 0; c < CHANNELS; c++) {
        for (int z = 0; wanted(c) && z < ZONES; z++) {
            int smp = preset.ch[c].s.zone[z].smp - 1;
            if (ready(smp)) {
                claim(smp);
            }
        }
    }
    for (int m = 0; m < SLICE_MAPS; m++) {
        if (maps[m].state != MAP_FREE && !maps[m].used) {
            maps[m].state = MAP_FREE;
            analysing = analysing == m ? -1 : analysing;
        }
    }
    if (analysing < 0) {
        for (int m = 0; m < SLICE_MAPS; m++) {
            if (maps[m].state == MAP_WAITING) {
                start(m);
                break;
            }
        }
    }
    if (analysing < 0) {
        return;
    }
    uint32_t begin = time_us();
    int done;
    do {
        done = hits_step();
    } while (!done && time_us() - begin < ANALYSIS_PASS_US);
    if (done) {
        finish(analysing);
    }
}

static const uint32_t *map_find(int smp, const struct sample *sm, uint32_t *count, uint32_t *analysed)
{
    for (int m = 0; m < SLICE_MAPS; m++) {
        const struct slice_map *map = &maps[m];
        if (map->state == MAP_READY && map->smp == smp && map->frame0 == sm->plane[0] && map->frames == sm->frames) {
            *count = map->count;
            *analysed = map->analysed;
            return map_positions[m];
        }
    }
    return 0;
}

int tlock_mapped(int ch, int zone)
{
    int c = preset_param_channel(ch), smp = preset.ch[c].s.zone[zone & (ZONES - 1)].smp - 1;
    const struct sample *sm = ready(smp);
    uint32_t n, a;

    return !sm ? -1 : map_find(smp, sm, &n, &a) ? 1 : 0;
}

/* ---- tempo (upstream ext/timelock.c's foreground) ---- */

static int tempo_channel(int c) { return preset.ch[c].tl.tempo == TIME_LOCK_TEMPO_CHANNEL_1 ? 0 : c; }

static unsigned tempo_of(int c)
{
    unsigned t = preset.ch[tempo_channel(c)].tl.tempo;

    return t == TIME_LOCK_TEMPO_CHANNEL_1 ? TIME_LOCK_TEMPO_OWN : t;
}

static uint32_t clock_ppqn(int c)
{
    int s = tempo_channel(c);

    if (tempo_of(c) == TIME_LOCK_TEMPO_MIDI) {
        return MIDI_PPQN;
    }
    return preset.ch[s].tl.ppqn ? preset.ch[s].tl.ppqn : TIME_LOCK_PPQN_DEFAULT;
}

static struct clock *clock_of(int c)
{
    unsigned t = tempo_of(c);

    return t == TIME_LOCK_TEMPO_MIDI ? &clocks[0] : t == TIME_LOCK_TEMPO_CV ? &clocks[1 + tempo_channel(c)] : 0;
}

static uint32_t clock_bpm10(struct clock *k, uint32_t ppqn)
{
    if (!k->period_q8) {
        return 0;
    }
    uint32_t raw = (uint32_t)((uint64_t)SECONDS_PER_MINUTE * HUNDREDTHS * OUTPUT_HZ * (1u << 8) /
                              ((uint64_t)k->period_q8 * ppqn));
    int32_t off = (int32_t)raw - (int32_t)(k->shown10 * TENTHS);
    if (off > BPM_HOLD_HUNDREDTHS || off < -BPM_HOLD_HUNDREDTHS) {
        k->shown10 = (raw + TENTHS / 2) / TENTHS;
    }
    return k->shown10;
}

/* The zone's sample and the frames its Beats span: its loop when it loops,
 * else start to end. */
static const struct sample *zone_span(int c, int z, uint32_t *span, int *smp)
{
    const struct zone_cfg *zc = &preset.ch[c].s.zone[z];
    const struct sample *sm = ready(zc->smp - 1);

    *span = 0;
    *smp = zc->smp - 1;
    if (!sm) {
        return 0;
    }
    uint32_t frames = sm->frames, start = zc->start, end = zc->end == POS_END || zc->end > frames ? frames : zc->end;
    uint32_t loop = zc->loop_start;
    uint32_t length = zc->loop_len == POS_END ? (loop < frames ? frames - loop : 0) : zc->loop_len >> 5;
    if (preset.ch[c].s.loop_mode != LOOP_OFF && loop >= start && length && loop + length <= end) {
        *span = length;
    } else {
        *span = end > start ? end - start : 0;
    }
    return sm;
}

void tlock_tempo(int ch, int zone, struct tlock_tempo *out)
{
    int c = preset_param_channel(ch), smp;
    struct clock *k = clock_of(c);
    uint32_t span;

    memset(out, 0, sizeof(*out));
    out->clocked = k != 0;
    out->clock_live = k && timelock_clock_live(k);
    if (out->clock_live) {
        out->bpm10 = clock_bpm10(k, clock_ppqn(c));
    }
    const struct sample *sm = preset.ch[c].machine ? 0 : zone_span(c, zone & (ZONES - 1), &span, &smp);
    if (!sm || !span || !sm->rate) {
        return;
    }
    uint32_t set = preset.ch[c].s.zone[zone & (ZONES - 1)].beats;
    out->guessed = !set;
    out->beats = set ? set : timelock_guess(sm->name, SAMPLE_NAME_LEN, span, sm->rate);
    out->own_bpm10 = (uint32_t)((uint64_t)SECONDS_PER_MINUTE * TENTHS * out->beats * sm->rate / span);
    unsigned t = tempo_of(c);
    if (t == TIME_LOCK_TEMPO_OWN) {
        out->bpm10 = out->own_bpm10;
    } else if (t == TIME_LOCK_TEMPO_SPEED) {
        out->bpm10 = (uint32_t)((uint64_t)out->own_bpm10 *
                                (uint32_t)(TIME_LOCK_SPEED_UNITY + preset.ch[tempo_channel(c)].tl.speed) /
                                TIME_LOCK_SPEED_UNITY);
    }
}

/* fw/tallent/mix_fx.c: the delay's beat in Q8 frames from MIDI clock; 0
 * when it has stopped. */
u32 ta_midi_beat_q8(void)
{
    return timelock_clock_live(&clocks[0]) ? clocks[0].period_q8 * MIDI_PPQN : 0;
}

u32 ta_clock_beat_q8(void)
{
    return timelock_clock_live(&delay_clock) ? delay_clock.period_q8 * delay_ppqn : 0;
}

u32 ta_clock_bpm10(void)
{
    int midi = mix_now()->delay_tempo == MIX_TEMPO_MIDI;
    struct clock *k = midi ? &clocks[0] : &delay_clock;

    return timelock_clock_live(k) ? clock_bpm10(k, midi ? MIDI_PPQN : delay_ppqn) : 0;
}

/* A zone as the interrupt plays it (upstream's zone_build): its sample
 * range from the record (the interrupt takes the voice's), the timeline's
 * speed at the channel's tempo. */
static void zone_build(struct lock_zone *out, uint32_t *analysed_out, int c, int z, int pair)
{
    const struct zone_cfg *zc = &preset.ch[c].s.zone[z];
    uint32_t span, count, analysed;
    int smp;
    const struct sample *sm = zone_span(c, z, &span, &smp);

    memset(out, 0, sizeof(*out));
    *analysed_out = 0;
    if (!sm || preset.ch[c].s.reverse) {
        return;
    }
    const uint32_t *positions = map_find(smp, sm, &count, &analysed);
    if (!positions || !count || !sm->rate) {
        return;
    }
    uint32_t frames = sm->frames, start = zc->start, end = zc->end == POS_END || zc->end > frames ? frames : zc->end;
    if (start >= end) {
        return;
    }
    out->pcm[0] = sm->plane[pair ? 0 : zc->side & 1];
    out->pcm[1] = pair && sm->channels == 2 ? sm->plane[1] : 0;
    out->positions = positions;
    out->count = count;
    out->first = start;
    out->end = end < analysed ? end : analysed;
    *analysed_out = analysed;
    if (out->end <= out->first + 2 * READ_MARGIN) {
        out->count = 0;
        return;
    }
    uint32_t loop = zc->loop_start;
    uint32_t length = zc->loop_len == POS_END ? (loop < frames ? frames - loop : 0) : zc->loop_len >> 5;
    out->turn = out->end;
    if (preset.ch[c].s.loop_mode != LOOP_OFF && loop >= start && length && loop + length <= out->end) {
        out->loop = loop;
        out->turn = loop + length;
    }
    uint64_t own = ((uint64_t)sm->rate << AUDIO_STEP_SHIFT) / OUTPUT_HZ, base = own;
    unsigned tempo = tempo_of(c);
    if (tempo == TIME_LOCK_TEMPO_SPEED) {
        base = own * (uint32_t)(TIME_LOCK_SPEED_UNITY + preset.ch[tempo_channel(c)].tl.speed) / TIME_LOCK_SPEED_UNITY;
    } else if (tempo != TIME_LOCK_TEMPO_OWN) {
        uint32_t beats = zc->beats ? zc->beats : timelock_guess(sm->name, SAMPLE_NAME_LEN, span, sm->rate);
        uint64_t ticks = (uint64_t)beats * clock_ppqn(c);
        out->tick = (s64)(((uint64_t)span << 32) / ticks);
        const struct clock *k = clock_of(c);
        /* Until the clock is measured: the sample's own speed. */
        if (k->period_q8) {
            base = ((uint64_t)span << (AUDIO_STEP_SHIFT + 8)) / (ticks * k->period_q8);
        }
        base = base > own << SPEED_RANGE_SHIFT ? own << SPEED_RANGE_SHIFT : base;
        base = base < (own >> SPEED_RANGE_SHIFT) + 1 ? (own >> SPEED_RANGE_SHIFT) + 1 : base;
    }
    out->base = (s32)base;
    out->inverse = (u32)((1ull << 56) / (u32)out->base);
    out->loop_index = (u16)(out->loop ? timelock_after(out, out->loop - 1u) : 0u);
}

/* Inputs by row (A, B, C) of eight channels -> cv_frames' 1A, 1B, 1C, 2A.. */
static int row_input(s32 source, int channel)
{
    if (source < EQ_CV_SOURCE_RELATIVE_FIRST || source > EQ_CV_SOURCE_MAX) {
        return -1;
    }
    int row = source < 0 ? source - EQ_CV_SOURCE_RELATIVE_FIRST : source % 3;
    int ch = source < 0 ? channel : source / 3;
    return ch * 3 + row;
}

void tlock_poll(void)
{
    if (!polled) {
        polled = 1;
        timelock_init();
    }
    maps_poll();
    for (unsigned n = 0; n < CLOCKS; n++) {
        uint32_t ppqn = !n ? MIDI_PPQN : preset.ch[n - 1].tl.ppqn ? preset.ch[n - 1].tl.ppqn : TIME_LOCK_PPQN_DEFAULT;
        uint32_t window = ppqn * CLOCK_BEATS, period = clocks[n].period_q8;
        clocks[n].window = (u8)(window < CLOCK_RING ? window : CLOCK_RING - 1);
        clocks[n].settle = (u8)(ppqn < CLOCK_RING ? ppqn : CLOCK_RING - 1);
        clocks[n].inverse = period ? (u32)((1ull << 40) / period) : 0;
    }
    const struct mix_settings *mx = mix_now();
    delay_ppqn = mx->delay_clk_ppqn ? mx->delay_clk_ppqn : TIME_LOCK_PPQN_DEFAULT;
    delay_clock.window = (u8)(delay_ppqn * CLOCK_BEATS < CLOCK_RING ? delay_ppqn * CLOCK_BEATS : CLOCK_RING - 1);
    delay_clock.settle = (u8)(delay_ppqn < CLOCK_RING ? delay_ppqn : CLOCK_RING - 1);
    delay_clock.inverse = delay_clock.period_q8 ? (u32)((1ull << 40) / delay_clock.period_q8) : 0;
    delay_input = mx->delay_tempo == MIX_TEMPO_CV && mx->delay_clk_src < MIX_CLK_INPUTS ? mx->delay_clk_src
                                                                                         : CLOCK_INPUT_NONE;
    uint32_t next = live ^ 1u;
    struct tl_tables *t = &tables[next];
    memset(t, 0, sizeof(*t));
    memset(t->clock_input, CLOCK_INPUT_NONE, sizeof(t->clock_input));
    for (int c = 0; c < CHANNELS; c++) {
        if (preset.ch[c].mode == MODE_STEREO && c) {
            continue;
        }
        /* Clocks run with Time Lock off too: channel 1 holds the tempo
         * others follow, and the delay can follow any channel's. */
        unsigned tempo = tempo_of(c);
        int from = tempo_channel(c), input = row_input(preset.ch[from].tl.input, from);
        if (tempo == TIME_LOCK_TEMPO_MIDI) {
            t->clock[c] = 1;
        } else if (tempo == TIME_LOCK_TEMPO_CV && input >= 0) {
            t->clock[c] = (uint8_t)(2 + from);
            t->clock_input[1 + from] = (uint8_t)input;
        }
        if (!wanted(c) || fx_grain(c)) {
            continue;
        }
        t->pair[c] = c + 1 < CHANNELS && preset.ch[c + 1].mode == MODE_STEREO;
        for (int z = 0; z < ZONES; z++) {
            zone_build(&t->zone[c][z], &t->analysed[c][z], c, z, t->pair[c]);
        }
        t->on[c] = 1;
    }
    if (memcmp(t, &tables[live], sizeof(*t))) {
        __asm__ volatile("dmb" ::: "memory");
        live = next;
    }
    const struct tl_tables *l = &tables[live];
    for (int c = 0; c < CHANNELS; c++) {
        if (l->on[c] && locks[c].engaged) {
            joins_plan(&locks[c], &l->zone[c][locks[c].zone & (ZONES - 1)], l->pair[c], steps_now[c]);
        }
    }
}

/* ---- audio interrupt ---- */

void tlock_block(unsigned frames, const int32_t (*in)[2])
{
    const struct tl_tables *t = &tables[live];
    uint32_t streams = 0;

    for (unsigned f = 0; f < frames; f++) {
        timelock_frames_now++;
        timelock_clock_frame(&clocks[0], 0);
        timelock_clock_now(&clocks[0], &clock_at[0][f]);
        for (unsigned n = 1; n < CLOCKS; n++) {
            if (t->clock_input[n] == CLOCK_INPUT_NONE) {
                continue;
            }
            float v = cv_frames(t->clock_input[n])[f] * (EQ_CV_COUNTS_FULL / 5.0f);
            s32 counts = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (s32)v;
            timelock_clock_frame(&clocks[n], timelock_clock_edge(&clocks[n], counts));
            timelock_clock_now(&clocks[n], &clock_at[n][f]);
        }
    }
    unsigned src = delay_input;
    if (src != CLOCK_INPUT_NONE) {
        const float *cv = src < MIX_CLK_INPUT_L ? cv_frames(src) : 0;
        for (unsigned f = 0; f < frames; f++) {
            /* The Sample input's pulse starts at 15% of full scale and ends at 7.5%. */
            s32 counts = cv ? (s32)(cv[f] * (EQ_CV_COUNTS_FULL / 5.0f)) : in[f][src - MIX_CLK_INPUT_L] >> 15;
            counts = counts > 32767 ? 32767 : counts < -32768 ? -32768 : counts;
            timelock_clock_frame(&delay_clock, timelock_clock_edge(&delay_clock, counts));
        }
    } else if (delay_clock.primed || delay_clock.period_q8) {
        memset(&delay_clock, 0, sizeof(delay_clock));
    }
    /* Taken once per 32 frames, as with 32-frame blocks. */
    static unsigned sub;
    if (sub++ % BLOCKS_PER_32) {
        return;
    }
    for (int c = 0; c < CHANNELS; c++) {
        if (locks[c].mixing) {
            streams += locks[c].streams[0].active + locks[c].streams[1].active;
        }
    }
    streams_now = streams;
}

void tlock_idle(int ch)
{
    if (locks[ch].engaged) {
        timelock_disengage(&locks[ch]);
    }
}

int tlock_pair(int ch)
{
    const struct tl_tables *t = &tables[live];

    return ch >= 0 && ch < CHANNELS && t->on[ch] && t->pair[ch];
}

int tlock_begin(int ch, int zone, int64_t start, int64_t end, int64_t lstart, int64_t lend, int looping,
                int32_t step, struct tlock_ch *out)
{
    const struct tl_tables *t = &tables[live];
    const struct lock_zone *tz = &t->zone[ch][zone & (ZONES - 1)];
    struct lock_zone *z = &work[ch];

    steps_now[ch] = step;
    if (!t->on[ch] || !tz->count) {
        tlock_idle(ch);
        return 0;
    }
    /* The voice's own range (its position mods included). */
    uint32_t first = (uint32_t)(start >> 32), last = (uint32_t)(end >> 32), analysed = t->analysed[ch][zone & (ZONES - 1)];
    uint32_t loop = 0, turn;
    last = last < analysed ? last : analysed;
    if (last <= first + 2 * READ_MARGIN) {
        tlock_idle(ch);
        return 0;
    }
    turn = last;
    if (looping && lstart >> 32 >= first && lend >> 32 > lstart >> 32 && (uint32_t)(lend >> 32) <= last) {
        loop = (uint32_t)(lstart >> 32);
        turn = (uint32_t)(lend >> 32);
    }
    if (z->positions != tz->positions || z->count != tz->count || z->first != first || z->end != last ||
        z->loop != loop || z->turn != turn || z->base != tz->base || z->tick != tz->tick || z->pcm[0] != tz->pcm[0] ||
        z->pcm[1] != tz->pcm[1]) {
        *z = *tz;
        z->first = first;
        z->end = last;
        z->loop = loop;
        z->turn = turn;
        z->loop_index = (u16)(loop ? timelock_after(z, loop - 1u) : 0u);
    }
    out->l = &locks[ch];
    out->z = z;
    out->zone = (uint32_t)zone;
    out->pair = t->pair[ch];
    out->own = step > 0 && z->base ? (uint32_t)(((uint64_t)z->base << 16) / (uint32_t)step) : 0;
    out->k = t->clock[ch] ? clock_at[t->clock[ch] - 1] : 0;
    out->streams = streams_now;
    return 1;
}
