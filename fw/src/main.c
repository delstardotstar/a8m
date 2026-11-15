#include "hw.h"
#include "oled.h"
#include "panel.h"
#include "systime.h"
#include "version.h"
#include "diag.h"
#include "audio.h"
#include "board.h"
#include "intc.h"
#include "mmu.h"
#include "cv.h"
#include "cal.h"
#include "engine.h"
#include "fx.h"
#include "health.h"
#include "prof.h"
#include "tlock.h"
#ifdef PROFILE_MIXFX
#include "mix_fx.h"
#endif
#include "preset.h"
#include "samples.h"
#include "rec.h"
#include "storage.h"
#include "ui.h"
#include "update.h"
#include "leds.h"
#include "voices.h"
#include "wdt.h"
#include "mdkit_tables.h"

/* At payload offset 0x40, right after the vectors (link.ld), so Load
 * Software can identify an image without running it. */
const char fw_version[32] __attribute__((section(".fwversion"), used)) = "A8M " FW_VERSION;

/* Flush denormals to zero (decaying voice tails would otherwise take the
 * VFP's slow path) and use the default NaN. */
static void fpu_fast_mode(void)
{
    uint32_t fpscr;

    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr |= (1u << 24) | (1u << 25);
    __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr));
}

#ifdef PROFILE_VOICE
/* Cost profiling (emu/voicecost.py): one second of voice PROFILE_VOICE
 * alone on channel 1 through the engine, retriggered 4 times, outside any
 * interrupt; then "DONE" on UART0. */
static void uart_num(const char *label, uint32_t v)
{
    char d[12];
    int n = 0;

    for (; *label; label++) {
        REG(0x44e09000u) = (uint32_t)*label;
    }
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        REG(0x44e09000u) = (uint32_t)d[--n];
    }
}

/* Raw voice output for emu/voicecost.py --check: default controls, one
 * trigger, CHECK_FRAMES samples rendered 32 at a time, as hex float bits.
 * Pitch-input voices start at middle C and move up CHECK_OCTAVES (Q16)
 * at CHECK_MOVE, as emu/mnmdref.cpp does. */
#define CHECK_FRAMES 22050
#define CHECK_MOVE 11008
#define CHECK_OCTAVES 38229
static uint8_t check_state[VOICE_STATE_MAX] __attribute__((aligned(16)));

static void check_dump(void)
{
    const struct voice_desc *vd = voices[PROFILE_VOICE];
    const mdkit_voice *v = vd->ops;
    float buf[32];

    v->init(check_state);
    for (unsigned i = 0; i < vd->nparams; i++) {
        v->set_control(check_state, i, (unsigned)vd->params[i].def);
    }
    v->trigger(check_state);
    uart_num("SAMPLES", 0);
    for (int n = 0; n < CHECK_FRAMES; n += 32) {
        if (n == CHECK_MOVE && (vd->flags & VF_PITCH_IN)) {
            voice_set_pitch(vd, check_state, CHECK_OCTAVES);
        }
        v->render(check_state, buf, 32);
        for (int k = 0; k < 32; k++) {
            union { float f; uint32_t u; } b = { buf[k] };
            REG(0x44e09000u) = ' ';
            for (int d = 7; d >= 0; d--) {
                REG(0x44e09000u) = (uint32_t)"0123456789abcdef"[(b.u >> (4 * d)) & 15];
            }
        }
    }
    uart_num("\nEND", 0);
}

#ifndef PROFILE_EVERY
#define PROFILE_EVERY 375
#endif
#ifndef PROFILE_PITCH
#define PROFILE_PITCH 0
#endif
#ifndef PROFILE_RATE
#define PROFILE_RATE 44100
#endif

/* emu/plugin/blockcost.c: r0 >= 0 opens a cost window labelled r0, < 0 closes it. */
__attribute__((noinline)) void profile_mark(int label)
{
    __asm__ volatile("" :: "r"(label));
}

static void profile_voice(void)
{
    static int32_t out[AUDIO_BLOCK][AUDIO_OUTPUTS], mix[AUDIO_BLOCK][2], in[AUDIO_BLOCK][2];

#ifdef PROFILE_CHECK
    check_dump();
    uart_num("\nDONE", 0);
    for (;;) {
    }
#endif
#if PROFILE_VOICE == CARD_PROFILE
    /* The prefs' folder and preset, loaded whole (presets and samples). */
    storage_init();
    for (int idle = 0, n = 0; idle < 50 && n < 200000; n++) {
        storage_poll();
        idle = storage_loading() || storage_samples_percent() >= 0 ? 0 : idle + 1;
    }
    uart_num("LOADED ", (uint32_t)preset_number);
#else
    for (int ch = 0; ch < CHANNELS; ch++) {
        preset.ch[ch].machine = 0;
    }
#endif
#if PROFILE_VOICE == CARD_PROFILE
#elif PROFILE_VOICE == SAMPLER_PROFILE || PROFILE_VOICE == SAMPLER_MOD_PROFILE || PROFILE_VOICE == SAMPLER_PM_PROFILE
    /* 8 looping sample voices at 44.1 kHz (resampled), each its own channel. */
    static int32_t plane[48000 + 2 * SAMPLE_GUARD] __attribute__((aligned(16)));
    float re = 1e9f, im = 0.0f;
    for (int f = 0; f < 48000; f++) {
        float t = re * 0.998f - im * 0.0627f;
        im = im * 0.998f + re * 0.0627f;
        re = t;
        plane[SAMPLE_GUARD + f] = (int32_t)im;
    }
#ifdef PROFILE_TLOCK
    /* A loop for Time Lock: a hit every 3000 frames, a thump and a burst. */
    {
        uint32_t r = 1;
        float env = 0.0f;
        for (int f = 0; f < 48000; f++) {
            if (f % 3000 == 0) {
                env = f % 12000 ? 0.5f : 0.9f;
            }
            r = r * 1664525u + 1013904223u;
            float x = env * (0.6f * (float)(int32_t)r * (1.0f / 2147483648.0f) + 0.4f * (float)(f % 109) / 54.5f - 0.4f);
            env *= 0.9995f;
            plane[SAMPLE_GUARD + f] = (int32_t)(x * 1.6e9f);
        }
    }
#endif
    samples_reset("PROFILE", 0);
    int si = samples_add("p.wav", 0);
    samples[si].rate = PROFILE_RATE;
    samples[si].frames = 48000;
    samples[si].plane[0] = samples[si].plane[1] = plane + SAMPLE_GUARD;
#if PROFILE_GRAIN == 2 || PROFILE_TLOCK == 2
    static int32_t plane_r[48000 + 2 * SAMPLE_GUARD] __attribute__((aligned(16)));
    for (int f = 0; f < 48000; f++) {
        plane_r[SAMPLE_GUARD + f] = plane[SAMPLE_GUARD + 47999 - f];
    }
    samples[si].plane[1] = plane_r + SAMPLE_GUARD;
    samples[si].channels = 2;
#endif
    samples[si].state = SMP_READY;
    for (int ch = 0; ch < CHANNELS; ch++) {
        struct sampler_cfg *sc = &preset.ch[ch].s;
        sampler_defaults(sc);
        sc->play_mode = PLAY_ONESHOT;
        sc->loop_mode = LOOP_NORMAL;
        sc->zone[0].loop_start = 1000;
        sc->zone[0].loop_len = 5000u * 32u;
        preset.ch[ch].pitch = (int16_t)(ch * 100 + PROFILE_PITCH);
        sc->zone[0].smp = (uint16_t)(si + 1);
#ifdef PROFILE_ALIAS
        /* Aliasing 0..PROFILE_ALIAS % over the channels, odd ones reversed. */
        sc->aliasing = (uint8_t)(PROFILE_ALIAS * ch / 7);
        sc->reverse = (uint8_t)(ch & 1);
#endif
#ifdef PROFILE_TLOCK
        /* Time Lock on every channel, pitched -8..+20 semitones, odd
         * channels at Speed 130%; PROFILE_TLOCK=2 makes 2, 4, 6 and 8
         * Stereo/Right of a stereo sample. */
        sc->zone[0].loop_start = 0;
        sc->zone[0].loop_len = 47000u * 32u;
        preset.ch[ch].pitch = (int16_t)((ch - 2) * 400);
        preset.ch[ch].tl.on = PROFILE_TLOCK != 0;   /* 0: the same scene unlocked */
        preset.ch[ch].tl.tempo = (uint8_t)(ch & 1 ? TIME_LOCK_TEMPO_SPEED : TIME_LOCK_TEMPO_OWN);
        preset.ch[ch].tl.speed = 300;
        if (PROFILE_TLOCK == 2 && (ch & 1)) {
            preset.ch[ch].mode = MODE_STEREO;
            sc->zone[0].side = 1;
        }
#endif
#ifdef PROFILE_GRAIN
        /* Grains on every channel, eight sounding, scattered; PROFILE_GRAIN=2
         * makes 2, 4, 6 and 8 Stereo/Right of a stereo sample. */
        {
            static const int16_t dense[TA_GRAIN_FIELDS] = { 1, 300, 800, 50, 30, 25, 25, 100, 60, 50, 0, -20 };
            memcpy(preset.ch[ch].grain.value, dense, sizeof(dense));
            if (PROFILE_GRAIN == 2 && (ch & 1)) {
                preset.ch[ch].mode = MODE_STEREO;
                sc->zone[0].side = 1;
            }
        }
#endif
#if PROFILE_VOICE == SAMPLER_MOD_PROFILE
        preset.ch[ch].mod[D_PITCH] = (struct mod){ CV_REL_A, 0, 50 };
        preset.ch[ch].mod[D_PAN] = (struct mod){ CV_REL_B, 0, 100 };
        sc->mod[SM_EXPAM] = (struct mod){ CV_REL_A, 0, 100 };
#endif
#if PROFILE_VOICE == SAMPLER_PM_PROFILE
        /* 1-4 phase modulated by 5-8, 5 by itself, 6 by CV A, 7 by the left
         * input (index CV-modulated); aliasing 40% on 1-4, smoothing on
         * 5-8 (short loops), all in two crossfade groups. */
        sc->pm_index = 20;
        sc->pm_source = (uint8_t)(ch < 4 ? ch + 4 : ch == 4 ? 4 : ch == 5 ? PM_CV : ch == 6 ? PM_LEFT : 0);
        sc->mod[SM_PHASE] = (struct mod){ CV_REL_A, 0, 100 };
        sc->mod[SM_PMINDEX] = (struct mod){ ch == 6 ? CV_REL_B : CV_OFF, 0, 100 };
        sc->aliasing = ch < 4 ? 40 : 0;
        sc->splice = ch >= 4;
        if (ch >= 4) {
            sc->zone[0].loop_len = 200u * 32u;
        }
        sc->xfade = (uint8_t)(1 + ch / 4);
        preset.xfade_cv[ch / 4] = (uint8_t)(1 + ch * 3);
#endif
    }
#elif PROFILE_VOICE == PI_HEAVY_PROFILE || PROFILE_VOICE == PI_SCENE_PROFILE
    /* PI HEAVY (fw/tools/presets.c); the scene adds the 2026-11-11 hardware
     * test's routes (CV A -> pitch, CV B -> bits on 1-5 and 7), Bits and the
     * machine envelope, at PROFILE_PITCH cents. */
    static const int heavy[CHANNELS] = { 23, 27, 24, 22, 25, 29, 30, 28 };
    for (int ch = 0; ch < CHANNELS; ch++) {
        struct channel_cfg *c = &preset.ch[ch];
        preset_set_machine(c, heavy[ch]);
        c->s.play_mode = PLAY_ONESHOT;
#if PROFILE_VOICE == PI_SCENE_PROFILE
        c->pitch = PROFILE_PITCH;
        c->bits = 120;
        c->s.attack = 20;
        if (ch < 5 || ch == 6) {
            c->mod[D_PITCH] = (struct mod){ CV_REL_A, 0, 50 };
            c->mod[D_BITS] = (struct mod){ CV_REL_B, 0, 100 };
        }
#endif
    }
#else
    /* 1-SHOT: sustaining machines play on between the hits. PROFILE_ALL8:
     * the voice on all 8 channels, each its own Master. */
#ifdef PROFILE_ALL8
    for (int ch = 0; ch < CHANNELS; ch++) {
#else
    for (int ch = 0; ch < 1; ch++) {
#endif
        preset_set_machine(&preset.ch[ch], PROFILE_VOICE);
        if (voices[PROFILE_VOICE]) {
            preset.ch[ch].s.play_mode = PLAY_ONESHOT;
        }
        preset.ch[ch].pitch = (int16_t)(ch * 100);
    }
#endif
#ifdef PROFILE_ONSETS
    /* START DELAY (with 99): a click on 1-6 and EFM-BD on 7-8, all fired
     * by channel 1; delays 0, 10, 0.3, 125, 2.0 ms; 6 and 8 Stereo/Right
     * of 5 and 7 with offsets +5.0 and +0.7 ms. Prints each channel's
     * first frame above -60 dB after the first trigger. */
    {
        static const uint16_t delay[CHANNELS] = { 0, 100, 3, 1250, 20, 0, 0, 0 };
        static const uint8_t mode[CHANNELS] = { MODE_MASTER, MODE_LINK, MODE_LINK, MODE_LINK, MODE_LINK,
                                                MODE_STEREO, MODE_LINK, MODE_STEREO };
        for (int f = 0; f < 48000; f++) {
            plane[SAMPLE_GUARD + f] = f < 24 ? 1932735283 : 0;
        }
        for (int ch = 0; ch < CHANNELS; ch++) {
            struct channel_cfg *c = &preset.ch[ch];
            c->pitch = 0;
            c->mode = mode[ch];
            c->s.loop_mode = LOOP_OFF;
            c->s.start_delay = delay[ch];
            if (ch >= 6) {
                preset_set_machine(c, 14);
                c->s.play_mode = PLAY_ONESHOT;
            }
        }
        preset.ch[5].ofs[PO_DELAY] = 50;
        preset.ch[7].ofs[PO_DELAY] = 7;
    }
#endif
#ifdef PROFILE_PAIR
    /* Pairs: 2, 4, 6 and 8 Stereo/Right of the channel before;
     * PROFILE_PAIR=2 gives the right sides offsets (pitch, level, two
     * controls, release, a 1 ms START DELAY). */
    for (int ch = 1; ch < CHANNELS; ch += 2) {
        preset.ch[ch].mode = MODE_STEREO;
#if PROFILE_PAIR == 2
        preset.ch[ch].ofs[PO_PITCH] = 700;
        preset.ch[ch].ofs[PO_LEVEL] = -30;
        preset.ch[ch].ofs[PO_CTRL] = 9;
        preset.ch[ch].ofs[PO_CTRL + 1] = -12;
        preset.ch[ch].ofs[PO_RELEASE] = 5;
        preset.ch[ch].ofs[PO_DELAY] = 10;
#endif
    }
#endif
#ifdef PROFILE_FX
    /* EQ (three bells and the HPF) and an LP 24 filter on all 8 channels;
     * PROFILE_FX=2 also moves each cutoff by CV A. */
    for (int ch = 0; ch < CHANNELS; ch++) {
        struct channel_cfg *c = &preset.ch[ch];
        c->eq.on = 1;
        c->eq.hpf = 40;
        c->eq.gain[0] = 30;
        c->eq.gain[1] = -45;
        c->eq.gain[2] = 20;
        c->eq.q[0] = c->eq.q[2] = 9;
        c->filter.type = 1;
        c->filter.resonance = 40;
#if PROFILE_FX == 2
        c->filter.source[0] = -3;
        c->filter.amount[0] = 50;
#endif
    }
#endif
#ifdef PROFILE_MIXFX
    /* Bits of PROFILE_MIXFX: 1 a compressor with drive on every channel
     * (channel 2 keyed by 1), 2 reverb sends (5 Rev+Cho, the hall and the
     * chorus), 4 delay sends, 8 the mix compressor. */
    {
        struct mix_settings *m = mix_edit();
        for (int ch = 0; ch < CHANNELS; ch++) {
            if (PROFILE_MIXFX & 1) {
                m->comp[ch] = (struct mix_comp_set){ 4, 24, 3, 3, 30, 6, 100, ch == 1 ? 1 : 0 };
            }
            m->reverb_send[ch] = PROFILE_MIXFX & 2 ? 40 : 0;
            m->delay_send[ch] = PROFILE_MIXFX & 4 ? 30 : 0;
        }
        m->reverb_algo = 4;
#ifdef PROFILE_WET
        m->reverb_level = 80;
        m->delay_level = 80;
        m->delay_feedback = 40;
#endif
#ifdef PROFILE_R80
        preset.reverb_unit = REVERB_80;
        preset.reverb_program = PROFILE_R80;
#endif
#ifdef PROFILE_R70
        preset.reverb_unit = REVERB_70;
        preset.reverb_program = PROFILE_R70;
#endif
        m->delay_tempo = MIX_TEMPO_BPM + 120 - MIX_BPM_MIN;
        if (PROFILE_MIXFX & 8) {
            m->master = (struct mix_comp_set){ 2, 12, 5, 4, 10, 3, 100, 0 };
        }
    }
#endif
    engine_service();
#ifdef INTERP_TEST
    {
        unsigned interp_selftest(unsigned rounds, unsigned *calls_direct);
        unsigned direct, bad = interp_selftest(INTERP_TEST, &direct);
        uart_num("INTERP_TEST BAD ", bad);
        uart_num(" DIRECT ", direct);
        uart_num("\n", 0);
    }
#endif
#if PROFILE_TLOCK
    while (tlock_mapped(0, 0) != 1) {
        engine_service();
    }
    engine_service();
#endif
#if PROFILE_VOICE == REC_PROFILE_192 || PROFILE_VOICE == REC_PROFILE_48
    static int32_t syn[4 * AUDIO_BLOCK][2];
    for (int f = 0; f < 4 * AUDIO_BLOCK; f++) {
        syn[f][0] = (f & 63) * 30000000 - 960000000;
        syn[f][1] = syn[f][0] / 2;
    }
    rec_setup.rate = PROFILE_VOICE == REC_PROFILE_192 ? RRATE_192 : RRATE_48;
    rec_setup.hpf = 1;
    rec_setup.level = 6;
    rec_metering(1);
    rec_poll();
#endif
#ifdef PROFILE_MONITOR
    /* The input path with the monitor on (as with SAMPLING SETUP's Monitor
     * L and R): 192 kHz frames from a write-through ring as input_block
     * reads them, capture at 48 kHz, the monitor into the mix. */
    static int32_t mring[4 * AUDIO_BLOCK][2] __attribute__((section(".dma"), aligned(64)));
    uint32_t mseed = 1;
    rec_setup.mon_l = rec_setup.mon_r = 1;
    rec_setup.mon_level = 0;
    rec_poll();
#endif
    uint32_t worst = 0, worst_block = 0, sum = 0;
#ifdef PROFILE_ONSETS
    uint32_t onset[AUDIO_OUTPUTS] = { 0 };
#endif
#ifdef PROFILE_HASH
    uint32_t hash = 0, chash[AUDIO_OUTPUTS + 2] = { 0 }, ihash = 0;
#endif

    for (int b = 0; b < AUDIO_RATE / AUDIO_BLOCK; b++) {
        if (b % (PROFILE_EVERY * BLOCKS_PER_32) == 0) {
            engine_trigger(0);
#if PROFILE_VOICE == SAMPLER_PROFILE || PROFILE_VOICE == SAMPLER_MOD_PROFILE || PROFILE_VOICE == SAMPLER_PM_PROFILE || \
    PROFILE_VOICE == PI_HEAVY_PROFILE || PROFILE_VOICE == PI_SCENE_PROFILE || PROFILE_VOICE == CARD_PROFILE || \
    defined(PROFILE_ALL8)
            for (int ch = 1; ch < CHANNELS; ch++) {
                engine_trigger(ch);
            }
#endif
        }
#ifdef PROFILE_MONITOR
        for (int f = 0; f < 4 * AUDIO_BLOCK; f++) {
            mseed = mseed * 1664525u + 1013904223u;
            mring[f][0] = (int32_t)(mseed & 0xffffff00u) >> 1;
            mring[f][1] = (int32_t)(((f + b * 4 * AUDIO_BLOCK) & 255) * 0x400000) - 0x20000000;
        }
#endif
        profile_mark(b);
        uint32_t t = time_ticks();
#if PROFILE_VOICE == REC_PROFILE_192 || PROFILE_VOICE == REC_PROFILE_48
        rec_input((const volatile int32_t (*)[2])syn, 4 * AUDIO_BLOCK, in);
#endif
#ifdef PROFILE_MONITOR
        dcache_inval_lines(mring, sizeof(mring));
        dcache_sync();
        preload_lines(mring, sizeof(mring));
        rec_input((const volatile int32_t (*)[2])mring, 4 * AUDIO_BLOCK, in);
#endif
        engine_render(out, mix, (const int32_t (*)[2])in, AUDIO_BLOCK);
#ifdef PROFILE_MONITOR
        rec_monitor(mix, AUDIO_BLOCK);
#endif
        t = time_ticks() - t;
#ifdef PROFILE_ONSETS
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            for (int k = 0; k < AUDIO_OUTPUTS; k++) {
                if (!onset[k] && (out[f][k] > 2147484 || out[f][k] < -2147484)) {
                    onset[k] = (uint32_t)(b * AUDIO_BLOCK + f + 1);
                }
            }
        }
#endif
#ifdef PROFILE_HASH
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            for (int k = 0; k < AUDIO_OUTPUTS; k++) {
                hash = hash * 31u + (uint32_t)out[f][k];
                chash[k] = chash[k] * 31u + (uint32_t)out[f][k];
            }
            hash = hash * 31u + (uint32_t)mix[f][0];
            hash = hash * 31u + (uint32_t)mix[f][1];
            chash[AUDIO_OUTPUTS] = chash[AUDIO_OUTPUTS] * 31u + (uint32_t)mix[f][0];
            chash[AUDIO_OUTPUTS + 1] = chash[AUDIO_OUTPUTS + 1] * 31u + (uint32_t)mix[f][1];
            ihash = (ihash * 31u + (uint32_t)in[f][0]) * 31u + (uint32_t)in[f][1];
        }
#endif
        profile_mark(-1);
        sum += t;
        if (t > worst) {
            worst = t;
            worst_block = (uint32_t)b;
        }
    }
    /* Under QEMU -icount the timer counts instructions (24 ticks/us). */
    uart_num("WORST ", worst);
    uart_num(" AT ", worst_block);
    uart_num(" SUM ", sum);
#ifdef PROFILE_HASH
    uart_num(" HASH ", hash);
    for (int k = 0; k < AUDIO_OUTPUTS + 2; k++) {
        uart_num(k ? " " : "\nCHASH ", chash[k]);
    }
    uart_num("\nIHASH ", ihash);
#endif
#ifdef PROFILE_ONSETS
    for (int k = 0; k < AUDIO_OUTPUTS; k++) {
        uart_num("\nONSET ", (uint32_t)k + 1);
        uart_num(" ", onset[k] ? onset[k] - 1 : 0);
    }
#endif
#ifdef TRIG_TRACE
    {
        extern uint32_t trig_trace[2][FAMILY_COUNT];
        for (int f = 0; f < FAMILY_COUNT; f++) {
            uart_num("\nTRIG ", (uint32_t)f);
            uart_num(" ", trig_trace[0][f]);
            uart_num(" LATE ", trig_trace[1][f]);
        }
        extern uint32_t chunk_hist[17], chunk_span[33];
        uart_num("\nTRIG CHUNKS", 0);
        for (int k = 0; k < 17; k++) {
            uart_num(" ", chunk_hist[k]);
        }
        uart_num("\nTRIG SPAN", 0);
        for (int k = 0; k < 33; k++) {
            uart_num(" ", chunk_span[k]);
        }
    }
#endif
#ifdef CHUNK_TRACE
    {
        extern uint16_t chunk_trace[4096];
        for (int b = 0; b < 4096; b++) {
            uart_num(b % 16 ? " " : "\nCT ", chunk_trace[b]);
        }
    }
#endif
    uart_num("\nDONE", 0);
    for (;;) {
    }
}
#endif

#ifdef TEST_FAULT
/* EXTRA=-DTEST_FAULT=n, 3 s after boot (2, 3, 5, 7 in the audio IRQ,
 * engine.c): 1 undefined instruction, 4 spin with IRQs masked, 6 spin
 * with IRQs on (main loop stall), 8 signed overflow (DEBUG=1: UBSan
 * trap), 9 stack smash (DEBUG=1), 10 read beyond DDR (DEBUG=1). */
static volatile uint32_t test_sink;

static void __attribute__((noinline, no_sanitize_undefined)) test_smash(void)
{
    char buf[8];
    volatile char *q = buf;

    for (volatile int i = 0; i < 64; i++) {
        q[i] = 0x55;
    }
    test_sink = (uint32_t)buf[0];
}

static void test_fault_main(void)
{
    static volatile int32_t big = 0x7fffffff;

    if (time_us() < 3000000u) {
        return;
    }
    switch (TEST_FAULT) {
    case 1:
        __builtin_trap();
    case 4:
        __asm__ volatile("cpsid i" ::: "memory");
        for (;;) {
        }
    case 6:
        for (;;) {
        }
    case 8:
        test_sink = (uint32_t)(big + 1);
        break;
    case 9:
        test_smash();
        break;
    case 10:
        test_sink = *(volatile uint32_t *)0xa0000000u;
        break;
    }
}
#endif

#ifdef TEST_HANG
/* EXTRA=-DTEST_HANG=n [-DTEST_HANG_AT=s]: s seconds after boot (default
 * 10), 1 spins with IRQs masked (audio stops too), 2 spins with IRQs on
 * (the main loop stalls, audio plays on). Not after a watchdog reset, so
 * the recovery shows. */
#ifndef TEST_HANG_AT
#define TEST_HANG_AT 10
#endif
static void test_hang_main(void)
{
    if (wdt_recovered() || time_us64() < TEST_HANG_AT * 1000000ull) {
        return;
    }
    if (TEST_HANG == 1) {
        __asm__ volatile("cpsid i" ::: "memory");
    }
    for (;;) {
    }
}
#endif

/* Preset changes by CV, applied as soon as the main loop sees them. */
static void cv_presets(void)
{
    if (storage_cv_poll()) {
        engine_service();
        storage_cv_latency_us = (time_ticks() - engine_status.preset_cv_ticks) / 24u;
    }
}

int main(void)
{
    struct panel p = { 0 };
    uint64_t next_draw = 0;

    diag_init();
    wdt_boot();
    diag_check_image();
    mmu_init();
    fpu_fast_mode();
    mdkit_tables_init();
    diag_stage(STAGE_MAIN);
    clocks_init();
    diag_step(0);           /* pad table */
    board_pads_init();
    diag_stage(STAGE_CLOCK_DOMAINS);
    time_init();
    diag_stage(STAGE_TIMER);
    oled_init();
    intc_init();
    diag_watchdog_init();
    panel_init();
    leds_init();
    preset_init(&preset);
#ifndef PROFILE_VOICE
    storage_init();
    cal_load();
#endif
#ifdef PROFILE_VOICE
    time_init();
    profile_voice();
#endif
#ifdef FX_TEST
    time_init();
    fx_selftest();
#endif
#ifdef MIXFX_TEST
    time_init();
    mixfx_selftest();
#endif
#ifdef COPY_TEST
    ui_copy_selftest();
#endif
    cv_init();
    engine_service();
    prof_init();
    diag_stage(STAGE_AUDIO);
    audio_start(engine_render);
    irq_on();
    diag_main_loop();
    diag_leds(1);           /* 1 SHOT: machines play one-shot */
    wdt_start();
    if (wdt_recovered()) {
        ui_say_for("RECOVERED FROM HANG", 8000000u);
        storage_count_hang();
        health_watchdog();
    }
    for (;;) {
        diag_alive();
        wdt_kick();
        DIAG_MAIN(DT_LOOP, 0);
#ifdef TEST_FAULT
        test_fault_main();
#endif
#ifdef TEST_HANG
        test_hang_main();
#endif
        DIAG_MAIN(DT_CV_PRESETS, 0);
        cv_presets();
        audio_poll_errors();
        audio_unmute_poll();
        DIAG_MAIN(DT_ENGINE_SERVICE, 0);
        engine_service();
        storage_poll();
        update_poll();
        cal_poll();
        DIAG_MAIN(DT_REC, 0);
        rec_poll();
        prof_poll();
        health_poll();
        engine_buttons = (uint8_t)(p.held >> BTN_CH1);
        leds_update(p.held);
        DIAG_MAIN(DT_PANEL, 0);
        panel_poll(&p);
        DIAG_MAIN(DT_UI_INPUT, 0);
        ui_input(&p);
        if (time_us64() >= next_draw) {
            next_draw = time_us64() + 33000;
            DIAG_MAIN(DT_UI_DRAW, 0);
            ui_draw();
            cv_presets();
        }
    }
}
