#ifdef MIXFX_TEST
/* EXTRA=-DMIXFX_TEST (emu/mixresp.py): the compressors, reverb and delay
 * measured through the renderer's stages; mixresp.py holds Tallent's model
 * of each and compares. Lines:
 *   S case ratio level-dB measured-cdB       static gain, channel or mix
 *   D drive level-dB measured-cdB            drive alone (Ratio Off)
 *   T kind index frames-x100                 63 % attack / release time
 *   V algo decay rt60-ms                     reverb (Schroeder, -5..-35 dB)
 *   G algo decay end-ms                      gated: energy 60 dB down
 *   W program 0 rt60-ms                      the 80's reverb, a factory program
 *                                            (-5..-25 dB, less the bus's floor)
 *   E case side frame amplitude-x1e4         delay echoes */
#include "engine.h"
#include "eq_design.h"
#include "fx.h"
#include "hw.h"
#include "mix_fx.h"
#include "preset.h"

static float chbuf[CHANNELS][AUDIO_BLOCK + 4] __attribute__((aligned(16)));
static const int32_t no_input[AUDIO_BLOCK][2];
static int32_t sends[AUDIO_BLOCK][CHANNELS], mixb[AUDIO_BLOCK][2];

static void put(const char *s)
{
    while (*s) {
        REG(0x44e09000u) = (uint32_t)*s++;
    }
}

static void put_int(int v)
{
    char d[12];
    int n = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;

    put(v < 0 ? " -" : " ");
    do {
        d[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n) {
        char c[2] = { d[--n], 0 };
        put(c);
    }
}

static void line(const char *tag, int a, int b, int c, int d)
{
    put(tag);
    put_int(a);
    put_int(b);
    put_int(c);
    if (d != -999999) {
        put_int(d);
    }
    put("\n");
}

static double db(double x)
{
    return 20.0 * logarithm(x < 1e-12 ? 1e-12 : x) / 2.302585092994046;
}

static double undb(double d)
{
    return exponential(d * 2.302585092994046 / 20.0);
}

/* One block of channel 1 at a constant level; the last frame's gain. */
static double chan_block(float level, double *gains)
{
    uint8_t lanes;

    for (int f = 0; f < AUDIO_BLOCK; f++) {
        chbuf[0][f] = level;
    }
    unsigned m = fx_mix_busy(AUDIO_BLOCK, &lanes);
    fx_render(chbuf, AUDIO_BLOCK, 0, lanes, m, no_input);
    for (int f = 0; gains && f < AUDIO_BLOCK; f++) {
        gains[f] = chbuf[0][f] / level;
    }
    return chbuf[0][AUDIO_BLOCK - 1] / level;
}

/* One block of the mix (both sides at level) and channel 1's send. */
static void mix_block(double level, int32_t send)
{
    uint8_t lanes;

    for (int f = 0; f < AUDIO_BLOCK; f++) {
        mixb[f][0] = mixb[f][1] = (int32_t)(level * 2147483647.0);
        for (int c = 0; c < CHANNELS; c++) {
            sends[f][c] = c ? 0 : send;
        }
        send = 0;
    }
    unsigned m = fx_mix_busy(AUDIO_BLOCK, &lanes);
    fx_mix_render(mixb, (const int32_t (*)[8])sends, AUDIO_BLOCK, m);
}

static struct mix_settings *fresh(void)
{
    struct mix_settings *m = mix_edit();

    *m = mix_defaults;
    m->format = MIX_FORMAT;
    return m;
}

static void statics(void)
{
    static const uint8_t ratios[] = { 2, 4, 8 };

    for (int k = 0; k < 3; k++) {
        struct mix_settings *m = fresh();
        m->comp[0] = (struct mix_comp_set){ ratios[k], 20, 0, 0, 0, 6, 100, 0 };
        fx_poll();
        for (int l = -40; l <= 0; l += 4) {
            double g = 1;
            for (int b = 0; b < (l == -40 ? 1000 : 100); b++) {
                g = chan_block((float)undb(l), 0);
            }
            line("S", 0, ratios[k], l, (int)(db(g) * 100.0 + (g < 1 ? -0.5 : 0.5)));
        }
    }
    /* The mix compressor, at the mix's own scale. */
    struct mix_settings *m = fresh();
    m->master = (struct mix_comp_set){ 4, 20, 0, 0, 0, 0, 100, 0 };
    fx_poll();
    for (int l = -40; l <= 0; l += 4) {
        for (int b = 0; b < 100 * BLOCKS_PER_32; b++) {
            mix_block(undb(l), 0);
        }
        double g = mixb[AUDIO_BLOCK - 1][0] / (undb(l) * 2147483647.0);
        line("S", 1, 4, l, (int)(db(g) * 100.0 + (g < 1 ? -0.5 : 0.5)));
    }
    /* Drive 100, Ratio Off. */
    m = fresh();
    m->comp[0] = (struct mix_comp_set){ 0, 20, 0, 0, 100, 0, 100, 0 };
    fx_poll();
    for (int l = -60; l <= 0; l += 12) {
        double g = 1;
        for (int b = 0; b < 4 * BLOCKS_PER_32; b++) {
            g = chan_block((float)undb(l), 0);
        }
        line("D", 100, l, (int)(db(g) * 100.0 + 0.5), -999999);
    }
}

/* Frames for the gain reduction to cover 63 % of a 20 dB step. */
static void timing(int release, int index)
{
    static double gains[AUDIO_BLOCK];
    struct mix_settings *m = fresh();
    float lo = (float)undb(-40), hi = 1.0f, a = release ? hi : lo, b = release ? lo : hi;

    m->comp[0] = (struct mix_comp_set){ 8, 20, (uint8_t)(release ? 0 : index), (uint8_t)(release ? index : 6), 0, 0, 100, 0 };
    fx_poll();
    for (int k = 0; k < 6000 * BLOCKS_PER_32; k++) {
        chan_block(a, 0);
    }
    double target = release ? 0 : -20.0, start = db(chan_block(a, 0)), mark = start + (target - start) * 0.632;
    double prev = start;
    for (int n = 0; n < 400000; n += AUDIO_BLOCK) {
        chan_block(b, gains);
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            double g = db(gains[f]);
            if (release ? g >= mark : g <= mark) {
                double frac = (mark - prev) / (g - prev);
                line("T", release, index, (int)((n + f + frac) * 100.0), -999999);
                return;
            }
            prev = g;
        }
    }
    line("T", release, index, -1, -999999);
}

/* Reverb: an impulse into the send, Schroeder decay of the return's energy
 * in 10 ms windows. */
#define WIN 480
#define WINS 1600
static double energy[WINS];

/* algo -1: the 80's program decay, -2 the 70's. Its fixed-point loop idles in a limit
 * cycle near -90 dBFS and the bus takes returns at 16 bits, so its decay is
 * measured over 25 dB, less the last second's floor. */
static void reverb(int algo, int decay)
{
    struct mix_settings *m = fresh();

    m->reverb_send[0] = 100;
    m->reverb_level = 100;
    if (algo < 0) {
        preset.reverb_unit = algo == -1 ? REVERB_80 : REVERB_70;
        preset.reverb_program = (uint8_t)decay;
        decay = 30;
    } else {
        preset.reverb_unit = 0;
        m->reverb_algo = (uint8_t)algo;
        m->reverb_decay = (uint8_t)decay;
    }
    fx_poll();
    for (int b = 0; b < 400 * BLOCKS_PER_32; b++) {
        mix_block(0, 0);
        fx_poll();
    }
    int wins = decay * 10 * 3 / 2 + 50;
    wins = wins > WINS ? WINS : wins;
    for (int w = 0; w < wins; w++) {
        energy[w] = 0;
    }
    for (int n = 0; n < wins * WIN; n += AUDIO_BLOCK) {
        mix_block(0, n ? 0 : algo < 0 ? 1 << 30 : 1 << 29);
        fx_poll();
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            double l = mixb[f][0], r = mixb[f][1];
            energy[(n + f) / WIN] += l * l + r * r;
        }
    }
    if (algo < 0) {
        double floor = 0;
        for (int w = wins - 100; w < wins; w++) {
            floor += energy[w] / 100;
        }
        for (int w = 0; w < wins; w++) {
            energy[w] = energy[w] > floor ? energy[w] - floor : 0;
        }
    }
    for (int w = wins - 2; w >= 0; w--) {
        energy[w] += energy[w + 1];
    }
    double e0 = energy[0], t5 = -1, t25 = -1, t35 = -1, t60 = -1;
    for (int w = 0; w < wins; w++) {
        double d = 10.0 * logarithm(energy[w] / e0 + 1e-30) / 2.302585092994046;
        if (t5 < 0 && d <= -5) t5 = w * 10.0;
        if (t25 < 0 && d <= -25) t25 = w * 10.0;
        if (t35 < 0 && d <= -35) t35 = w * 10.0;
        if (t60 < 0 && d <= -60) t60 = w * 10.0;
    }
    if (algo < 0) {
        line(algo == -1 ? "W" : "X", preset.reverb_program, 0, t25 < 0 ? -1 : (int)((t25 - t5) * 3.0), -999999);
        preset.reverb_unit = 0;
    } else if (algo >= (int)MIX_ALGO_GATED) {
        line("G", algo, decay, (int)t60, -999999);
    } else {
        line("V", algo, decay, t35 < 0 ? -1 : (int)((t35 - t5) * 2.0), -999999);
    }
}

/* Delay: an impulse into the send; the first three echoes' frames and
 * amplitudes, alternating sides. */
static void delay(int id, int division, int bpm, int feedback)
{
    struct mix_settings *m = fresh();

    m->delay_send[0] = 100;
    m->delay_level = 100;
    m->delay_feedback = (uint8_t)feedback;
    m->delay_low = 0;
    m->delay_high = MIX_FILTER_STEPS - 1;
    m->delay_division = (uint8_t)division;
    m->delay_tempo = (uint8_t)(MIX_TEMPO_BPM + bpm - MIX_BPM_MIN);
    fx_poll();
    for (int b = 0; b < 3000 * BLOCKS_PER_32; b++) {
        mix_block(0, 0);
    }
    int best[3] = { 0, 0, 0 }, at[3] = { 0, 0, 0 }, side[3] = { 0, 1, 0 };
    int span = 3 * 65536 + 1024, echo = 0;
    for (int n = 0; n < span; n += AUDIO_BLOCK) {
        mix_block(0, n ? 0 : 1 << 28);
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            for (int s = 0; s < 2; s++) {
                int v = mixb[f][s] < 0 ? -mixb[f][s] : mixb[f][s];
                /* Echoes follow each other; one ends where the next side starts. */
                if (echo < 3 && s == side[echo] && v > best[echo]) {
                    best[echo] = v;
                    at[echo] = n + f;
                } else if (echo < 2 && s == side[echo + 1] && v > best[echo] / 4) {
                    echo++;
                    best[echo] = v;
                    at[echo] = n + f;
                }
            }
        }
    }
    for (int e = 0; e < 3; e++) {
        line("E", id, side[e], at[e], (int)((double)best[e] / (1 << 28) * 10000.0 + 0.5));
    }
}

void mixfx_selftest(void)
{
    statics();
    static const uint8_t attacks[] = { 2, 4, 6, 8 }, releases[] = { 1, 2, 4 };
    for (unsigned i = 0; i < sizeof(attacks); i++) {
        timing(0, attacks[i]);
    }
    for (unsigned i = 0; i < sizeof(releases); i++) {
        timing(1, releases[i]);
    }
    reverb(1, 10);
    reverb(1, 30);
    reverb(2, 20);
    reverb(6, 50);
    reverb(9, 20);
    reverb(-1, 170);
    reverb(-1, 151);
    reverb(-1, 152);
    reverb(-1, 159);
    reverb(-2, 21);
    reverb(-2, 26);
    reverb(-2, 34);
    reverb(-2, 70);
    reverb(-2, 84);
    reverb(-1, 163);
    delay(0, 3, 120, 50);
    delay(1, 1, 100, 50);
    delay(2, 10, 40, 80);
    delay(3, 8, 133, 100);
    put("DONE\n");
    for (;;) {
    }
}
#endif
