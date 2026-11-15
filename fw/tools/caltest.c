/* Calibrate's loopback against a model of the hardware's signs: the
 * sample input stage and the mix output stage invert, channel outputs and
 * the CV inputs (in c) do not. A DC error on any output must come out
 * nulled, and the test patterns must read as connected. */
#include <stdio.h>
#include <stdlib.h>
#include "cal.h"
#include "audio.h"

#define WPV (2147483648.0 / 10.0)       /* words per volt at the jacks */
#define CPV 6553.6                      /* c per volt */

static int fails;
static double lg = 1.0;

static void check(int ok, const char *what, double v)
{
    if (!ok) {
        printf("FAIL %s (%g)\n", what, v);
        fails++;
    }
}

/* Volts at the mix jack for a mix bus value, given the offset. */
static double mix_jack(int32_t off, int32_t mix, double err)
{
    int32_t w = vget_lane_s32(cal_mix_words(vdup_n_s32(cal_mix_dc(off)), vdup_n_s32(mix)), 0);
    return -w / WPV + err;
}

/* The sample input's reading for volts at its jack (converter DC adc). */
static int32_t reading(double volts, double adc, int32_t off)
{
    double raw = -(volts * lg * WPV) + adc * WPV;
    int32_t r = (int32_t)(raw > 2147483647.0 ? 2147483647.0 : raw < -2147483648.0 ? -2147483648.0 : raw);
    return vgetq_lane_s32(cal_in_words(vdupq_n_s32(off), vdupq_n_s32(r)), 0);
}

static int ch_c(int8_t byte, double err)
{
    double v = byte * 65536.0 / WPV + err;
    return (int)(v * lg * CPV);
}

int main(void)
{
    static const double errs[] = { 0.02, -0.0387, 0.0, 0.15, -0.15, 0.25, -0.25, 9.94 };
    static const double adcs[] = { 0.0515, -0.03, 0.0 };
    int v;

    /* Polarity: what goes in comes out the same way round. */
    check(reading(1.0, 0.0, 0) > 0, "input +1 V reads positive", reading(1.0, 0.0, 0));
    check(mix_jack(0, 1 << 28, 0.0) > 0, "mix bus + gives + volts", mix_jack(0, 1 << 28, 0.0));
    for (unsigned a = 0; a < sizeof(adcs) / sizeof(adcs[0]); a++) {
        int64_t s = 0;
        for (int k = 0; k < CAL_SAMPLES; k++) {
            s += reading(0.0, adcs[a], 0);
        }
        int32_t in_off = cal_in_offset(s, &v);
        check(v, "input offset valid", adcs[a]);
        check(abs(reading(0.0, adcs[a], in_off)) < 64, "input nulled", reading(0.0, adcs[a], in_off));
        for (unsigned e = 0; e < sizeof(errs) / sizeof(errs[0]); e++) {
            double err = errs[e];
            int conn = 1;
            for (int up = 0; up < 2; up++) {
                int32_t o = up ? CAL_MIX_TEST : -CAL_MIX_TEST;
                conn &= cal_mix_responds(up, reading(mix_jack(o, 0, err), adcs[a], in_off));
            }
            s = 0;
            for (int k = 0; k < CAL_SAMPLES; k++) {
                s += reading(mix_jack(0, 0, err), adcs[a], in_off);
            }
            int32_t mo = cal_mix_offset(s, &v);
            if (err > 0.2 || err < -0.2) {
                check(!(conn && v), "large mix DC rejected", err);
                continue;
            }
            check(conn, "mix patterns connected", err);
            check(v, "mix offset valid", err);
            check(mix_jack(mo, 0, err) * 1000.0 < 0.01 && mix_jack(mo, 0, err) * 1000.0 > -0.01, "mix nulled (mV)",
                  mix_jack(mo, 0, err) * 1000.0);
        }
    }
    for (double err = -0.03; err <= 0.03; err += 0.0025) {
        int conn = cal_ch_responds(1, ch_c(127, err)) && cal_ch_responds(0, ch_c(-127, err));
        int64_t s = (int64_t)ch_c(0, err) * AUDIO_BLOCK * CAL_SAMPLES;
        int b = cal_ch_offset(s, &v);
        if (err > 0.021 || err < -0.021) {
            check(!(conn && v), "large channel DC rejected", err);
            continue;
        }
        check(conn, "channel patterns connected", err);
        check(v, "channel byte valid", err);
        check(ch_c((int8_t)b, err) <= 2 && ch_c((int8_t)b, err) >= -2, "channel nulled (c)", ch_c((int8_t)b, err));
    }
    for (unsigned gi = 0; gi < 3; gi++) {
        static const double gains[] = { 0.8, 1.25, 1.0 };
        lg = gains[gi];
        for (double err = -0.19; err <= 0.19; err += 0.019) {
            struct cal_iter it;
            int n = 0;
            if (err * lg > 0.19 || err * lg < -0.19) {
                continue;
            }
            cal_iter_init(&it, 1);
            while (!it.done && n < CAL_OUT_PASSES) {
                int64_t s = 0;
                for (int k = 0; k < CAL_SAMPLES; k++) {
                    s += reading(mix_jack(it.off, 0, err), 0.0, 0);
                }
                cal_mix_iter(&it, s);
                n++;
            }
            check(it.valid, "iterated mix valid", err);
            check(mix_jack(it.off, 0, err) * 1000.0 < 1.0 && mix_jack(it.off, 0, err) * 1000.0 > -1.0,
                  "iterated mix under 1 mV", mix_jack(it.off, 0, err) * 1000.0);
            check(abs(it.off) <= CAL_MIX_TEST, "iterated mix in range", it.off);
        }
        for (double err = -0.02; err <= 0.02; err += 0.002) {
            struct cal_iter it;
            int n = 0;
            if (err * lg > 0.019 || err * lg < -0.019) {
                continue;
            }
            cal_iter_init(&it, 1);
            while (!it.done && n < CAL_OUT_PASSES) {
                cal_ch_iter(&it, (int64_t)ch_c((int8_t)it.off, err) * AUDIO_BLOCK * CAL_SAMPLES);
                n++;
            }
            check(it.valid, "iterated channel valid", err);
            check(abs(ch_c((int8_t)it.off, err)) <= 6, "iterated channel under 1 mV (c)", ch_c((int8_t)it.off, err));
        }
    }
    /* Gain 2.5 over-corrects every pass: the first (stock) result is the best
     * and is kept. Gain -1 or 0 never improves. */
    {
        static const double bad[] = { 2.5, -1.0, 0.0 };
        for (unsigned gi = 0; gi < 3; gi++) {
            struct cal_iter it;
            int n = 0;
            lg = bad[gi];
            cal_iter_init(&it, 1);
            while (!it.done && n < CAL_OUT_PASSES) {
                int64_t s = 0;
                for (int k = 0; k < CAL_SAMPLES; k++) {
                    s += reading(mix_jack(it.off, 0, 0.1), 0.0, 0);
                }
                cal_mix_iter(&it, s);
                n++;
            }
            check(it.done && n <= 3, "diverging loop stops", n);
            check(it.off == it.best_off, "best kept", it.off);
            check(abs(it.resid) <= abs(reading(mix_jack(0, 0, 0.1), 0.0, 0)), "no worse than uncorrected", it.resid);
        }
        lg = 1.0;
    }
    printf(fails ? "caltest: %d failures\n" : "caltest: ok\n", fails);
    return fails != 0;
}
