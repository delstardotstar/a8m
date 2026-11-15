#include "fx.h"
#include "cv.h"
#include "engine.h"
#include "eq.h"
#include "grains.h"
#include "mix_fx.h"
#include "preset.h"
#include "r70.h"
#include "r80.h"
#include "rec.h"
#include "systime.h"
#include "tlock.h"

#include <arm_neon.h>

/* The DSP's samples are Q31; ours enter at 2^30 so a channel keeps 6 dB
 * above full scale for the mix, as the float path does. */
#define FX_SCALE        1073741824.0f

/* The reverb and delay run on this long after their sends end and their
 * returns fall quiet (a delay line's length and more). */
#define BUS_TAIL_BLOCKS (AUDIO_RATE / AUDIO_BLOCK * 3 / 2)
#define BUS_QUIET_Q15   2

int16_t ta_cv_counts[EQ_CV_INPUTS];
static int32_t xbuf[AUDIO_BLOCK][CHANNELS] __attribute__((aligned(16)));

/* What the mix effects' settings switch on, from the main loop. */
static volatile uint8_t mix_lanes, mix_flags;
static uint16_t bus_quiet = BUS_TAIL_BLOCKS;
static uint8_t mix_seen[sizeof(struct mix_settings)];
static int mix_ready;

u32 ta_master(u32 channel)
{
    return (u32)preset_param_channel((int)channel);
}

static const struct zone_cfg *playing_zone(u32 master)
{
    const struct channel_cfg *c = &preset.ch[master];

    return c->machine ? 0 : &c->s.zone[engine_status.zone[master] & (ZONES - 1)];
}

const struct eq_value *ta_eq_base(u32 master)
{
    const struct zone_cfg *z = playing_zone(master);

    return z && z->eq.override ? &z->eq : &preset.ch[master].eq;
}

const struct filter_value *ta_filter_base(u32 master)
{
    const struct zone_cfg *z = playing_zone(master);

    return z && z->filter.override ? &z->filter : &preset.ch[master].filter;
}

void ta_eq_right(u32 channel, struct eq_value *eq)
{
    const int16_t *o = preset.ch[channel].ofs + PO_EQGAIN;

    for (u32 b = 0; b < 3; b++) {
        if (o[b]) {
            s32 lim = eq_gain_limit(eq, b), g = eq->gain[b] + o[b];
            eq->gain[b] = (int16_t)(g < -lim ? -lim : g > lim ? lim : g);
        }
    }
}

u32 ta_ticks(void)
{
    return (u32)(time_us64() / 1000u);
}

/* Inputs by row (A, B, C) of eight channels, as the DSP reads them. */
static void cv_counts(void)
{
    const float32x4_t lo = vdupq_n_f32(-32768.0f), hi = vdupq_n_f32(32767.0f), k = vdupq_n_f32(EQ_CV_COUNTS_FULL / 5.0f);

    for (int ch = 0; ch < CHANNELS; ch += 4) {
        float32x4x3_t v = vld3q_f32((const float *)&cv_volts[ch * 3]);

        for (int row = 0; row < 3; row++) {
            float32x4_t c = vminq_f32(vmaxq_f32(vmulq_f32(v.val[row], k), lo), hi);
            vst1_s16(&ta_cv_counts[row * CHANNELS + ch], vmovn_s32(vcvtq_s32_f32(c)));
        }
    }
}

int ta_pair_right(u32 channel)
{
    return channel < CHANNELS && preset.ch[channel].mode == MODE_STEREO;
}

static int routed(const struct mix_route *r)
{
    return r->source != EQ_CV_SOURCE_OFF && r->amount;
}

/* Each channel's grain fields with their CV, derived when they change
 * into the tuning the renderer does not hold. */
static struct {
    struct grain_tuning t[2];
    s32 values[GRAIN_FIELDS];
    volatile uint8_t live, on;
} grain_pub[CHANNELS];

static void grain_poll(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        const struct channel_cfg *c = &preset.ch[ch];
        s32 v[GRAIN_FIELDS];

        if (c->machine || c->mode == MODE_STEREO || !c->grain.value[GRAIN_ON]) {
            grain_pub[ch].on = 0;
            continue;
        }
        grain_fields(&c->grain, (u32)ch, v);
        if (grain_pub[ch].on && !memcmp(v, grain_pub[ch].values, sizeof(v))) {
            continue;
        }
        unsigned next = grain_pub[ch].on ? grain_pub[ch].live ^ 1u : grain_pub[ch].live;
        memcpy(grain_pub[ch].values, v, sizeof(v));
        grain_derive(v, &grain_pub[ch].t[next]);
        __asm__ volatile("dmb" ::: "memory");
        grain_pub[ch].live = (uint8_t)next;
        grain_pub[ch].on = 1;
    }
}

const struct grain_tuning *fx_grain(int channel)
{
    return grain_pub[channel].on ? &grain_pub[channel].t[grain_pub[channel].live] : 0;
}

static void mix_poll(void)
{
    const struct mix_settings *m = mix_now();
    unsigned lanes = 0, flags = 0;

    if (!mix_ready) {
        mix_fx_init();
        mix_ready = 1;
    }
    if (memcmp(mix_seen, m, sizeof(mix_seen))) {
        memcpy(mix_seen, m, sizeof(mix_seen));
        mix_fx_update();
    }
    mix_fx_foreground();
    for (u32 c = 0; c < CHANNELS; c++) {
        if (ta_pair_right(c)) {
            continue;
        }
        if (m->comp[c].ratio || m->comp[c].drive) {
            lanes |= (ta_pair_right(c + 1) ? 3u : 1u) << c;
        }
        if (m->reverb_send[c] || m->delay_send[c] || routed(&m->send_route[c][MIX_ROUTE_REVERB_SEND]) ||
            routed(&m->send_route[c][MIX_ROUTE_DELAY_SEND])) {
            flags |= FXM_EFFECTS;
        }
    }
    if (rec_monitor_sends()) {
        flags |= FXM_EFFECTS;
    }
    if (m->master.ratio || m->master.drive) {
        flags |= FXM_MIX;
    }
    /* A sidechain off another channel or the input reads the channel stage's keys. */
    if (lanes || (m->master.ratio && m->master.key)) {
        flags |= FXM_CHANNELS;
    }
    mix_lanes = (uint8_t)lanes;
    __asm__ volatile("dmb" ::: "memory");
    mix_flags = (uint8_t)flags;
}

/* The 80's program, its edits and their CV routes (as the bus routes: at
 * 100, 5 V crosses the range). The laws run here, out of the audio. */
static void reverb80_poll(void)
{
    static const struct r80_program *live;
    static int16_t base[R80_EDITS];
    const struct r80_program *p = preset.reverb_unit == REVERB_80 ? r80_find(preset.reverb_program) : 0;

    if (p != live) {
        live = p;
        r80_load(p);
        for (unsigned e = 0; p && e < R80_EDITS; e++) {
            base[e] = -1;
        }
    }
    if (!p) {
        return;
    }
    for (unsigned e = 0; e < R80_EDITS && e < REVERB_EDITS; e++) {
        const struct reverb_route *r = &preset.reverb_route[e];
        int v = preset.reverb_edit[e];
        s32 in = r->amount ? eq_cv_input(r->source, 0) : -1;

        if (in >= 0) {
            int lo, hi;
            if (v < 0) {
                if (base[e] < 0) {
                    base[e] = (int16_t)r80_edit_base(p, e);
                }
                v = base[e];
            }
            r80_edit_range(p, e, &lo, &hi);
            v += r->amount * ta_cv_counts[in] / 100 * (hi - lo) / 32768;
            v = v < lo ? lo : v > hi ? hi : v;
        }
        r80_set(e, v);
    }
    r80_poll();
}

/* The 70's, the same way. */
static void reverb70_poll(void)
{
    static const struct r70_program *live;
    static int16_t base[R70_EDITS];
    const struct r70_program *p = preset.reverb_unit == REVERB_70 ? r70_find(preset.reverb_program) : 0;

    if (p != live) {
        live = p;
        r70_load(p);
        for (unsigned e = 0; e < R70_EDITS; e++) {
            base[e] = -1;
        }
    }
    if (!p) {
        return;
    }
    for (unsigned e = 0; e < R70_EDITS && e < REVERB_EDITS; e++) {
        const struct reverb_route *r = &preset.reverb_route[e];
        int v = preset.reverb_edit[e];
        s32 in = r->amount ? eq_cv_input(r->source, 0) : -1;

        if (in >= 0) {
            int lo, hi;
            if (v < 0) {
                if (base[e] < 0) {
                    base[e] = (int16_t)r70_edit_base(p, e);
                }
                v = base[e];
            }
            r70_edit_range(p, e, &lo, &hi);
            v += r->amount * ta_cv_counts[in] / 100 * (hi - lo) / 32768;
            v = v < lo ? lo : v > hi ? hi : v;
        }
        r70_set(e, v);
    }
    r70_poll();
}

void fx_poll(void)
{
    cv_counts();
    eq_foreground_poll();
    filter_foreground_poll();
    grain_poll();
    tlock_poll();
    mix_poll();
    reverb80_poll();
    reverb70_poll();
}

unsigned fx_mix_busy(unsigned frames, uint8_t *comp_lanes)
{
    unsigned f = mix_flags;

    if (!(f & FXM_EFFECTS) && bus_quiet < BUS_TAIL_BLOCKS) {
        f |= FXM_EFFECTS;
    }
    *comp_lanes = (f & FXM_CHANNELS) ? mix_lanes : 0;
    if (f) {
        cv_counts();
        ta_mix_block(frames);
    }
    return f;
}

int fx_send_pre(int channel)
{
    return mix_now()->send_pre[preset_param_channel(channel)] != 0;
}

void fx_mix_render(int32_t (*mix)[2], const int32_t (*send)[8], unsigned frames, unsigned flags)
{
    unsigned peak = ta_mix_bus(mix, send, frames, (flags & FXM_EFFECTS) != 0);

    if (!(flags & FXM_EFFECTS)) {
        return;
    }
    if (peak > BUS_QUIET_Q15 || (mix_flags & FXM_EFFECTS)) {
        bus_quiet = 0;
    } else if (bus_quiet < BUS_TAIL_BLOCKS) {
        bus_quiet++;
    }
}

void ta_mix_monitor(s32 (*send)[2], u32 frames)
{
    rec_monitor_send(send, frames);
}

/* The 70's or the 80's reverb in the DEP-5's place. */
u32 ta_reverb80(const s32 (*send)[2], s32 (*wet)[2], u32 frames)
{
    if (preset.reverb_unit == REVERB_80) {
        r80_block(send, wet, frames);
        return 1;
    }
    if (preset.reverb_unit == REVERB_70) {
        r70_block(send, wet, frames);
        return 1;
    }
    return 0;
}

void fx_flush(void)
{
    memset(filter_state, 0, sizeof(filter_state));
    for (int g = 0; g < 2; g++) {
        for (int k = 0; k < 5; k++) {
            memset(eq_fixed_data[g].section[k].history, 0, sizeof(eq_fixed_data[g].section[k].history));
        }
    }
    if (mix_ready) {
        mix_fx_flush();
    }
    r80_flush();
    r70_flush();
}

unsigned fx_reduction_db(int compressor)
{
    return mix_ready ? mix_fx_reduction_db((u32)compressor) : 0;
}

static unsigned lanes4(const int32_t *v)
{
    return (v[0] ? 1u : 0u) | (v[1] ? 2u : 0u) | (v[2] ? 4u : 0u) | (v[3] ? 8u : 0u);
}

int fx_busy(uint8_t *lanes)
{
    const struct filter_bank *fb[2] = { filter_mailbox.active, filter_mailbox.pending };
    const struct eq_bank *eb[2] = { eq_mailbox.active, eq_mailbox.pending };
    unsigned l = 0;

    for (int g = 0; g < 2; g++) {
        unsigned m = lanes4(eq_fixed_data[g].mask) | lanes4(filter_state[g][4]);

        for (int k = 0; k < 2; k++) {
            m |= (fb[k] ? lanes4(fb[k]->vector[g][FILTER_VECTOR_MASK]) : 0u) |
                 (eb[k] ? lanes4(eb[k]->group[g].mask) : 0u);
        }
        l |= m << (4 * g);
    }
    *lanes = (uint8_t)l;
    return l || eq_mailbox.pending || eq_mailbox.remaining || filter_mailbox.pending;
}

/* 4x4 transpose: rows a..d become columns. */
static inline void transpose4(int32x4_t *a, int32x4_t *b, int32x4_t *c, int32x4_t *d)
{
    int32x4x2_t ab = vtrnq_s32(*a, *b), cd = vtrnq_s32(*c, *d);

    *a = vcombine_s32(vget_low_s32(ab.val[0]), vget_low_s32(cd.val[0]));
    *b = vcombine_s32(vget_low_s32(ab.val[1]), vget_low_s32(cd.val[1]));
    *c = vcombine_s32(vget_high_s32(ab.val[0]), vget_high_s32(cd.val[0]));
    *d = vcombine_s32(vget_high_s32(ab.val[1]), vget_high_s32(cd.val[1]));
}

void fx_render(float (*buf)[AUDIO_BLOCK + 4], unsigned frames, int fx, uint8_t lanes, unsigned mix,
               const int32_t (*in)[2])
{
    const float32x4_t k = vdupq_n_f32(FX_SCALE), ik = vdupq_n_f32(1.0f / FX_SCALE);

    if (fx) {
        cv_counts();
    }
    for (int g = 0; g < CHANNELS; g += 4) {
        for (unsigned f = 0; f < frames; f += 4) {
            int32x4_t a = vcvtq_s32_f32(vmulq_f32(vld1q_f32(buf[g] + f), k));
            int32x4_t b = vcvtq_s32_f32(vmulq_f32(vld1q_f32(buf[g + 1] + f), k));
            int32x4_t c = vcvtq_s32_f32(vmulq_f32(vld1q_f32(buf[g + 2] + f), k));
            int32x4_t d = vcvtq_s32_f32(vmulq_f32(vld1q_f32(buf[g + 3] + f), k));

            transpose4(&a, &b, &c, &d);
            vst1q_s32(&xbuf[f][g], a);
            vst1q_s32(&xbuf[f + 1][g], b);
            vst1q_s32(&xbuf[f + 2][g], c);
            vst1q_s32(&xbuf[f + 3][g], d);
        }
    }
    if (fx) {
        ta_fx_run(xbuf, frames);
    }
    if (mix & FXM_CHANNELS) {
        ta_mix_channels(xbuf, in, frames);
    }
    for (int g = 0; g < CHANNELS; g += 4) {
        for (unsigned f = 0; f < frames; f += 4) {
            int32x4_t r[4] = { vld1q_s32(&xbuf[f][g]), vld1q_s32(&xbuf[f + 1][g]), vld1q_s32(&xbuf[f + 2][g]),
                               vld1q_s32(&xbuf[f + 3][g]) };

            transpose4(&r[0], &r[1], &r[2], &r[3]);
            for (int j = 0; j < 4; j++) {
                if ((lanes >> (g + j)) & 1) {
                    vst1q_f32(buf[g + j] + f, vmulq_f32(vcvtq_f32_s32(r[j]), ik));
                }
            }
        }
    }
}
