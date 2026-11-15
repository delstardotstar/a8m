#include "cal.h"
#include "audio.h"

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

int cal_zero_offset(int64_t frames_sum, int *valid)
{
    int64_t h = floor_div(frames_sum, (int64_t)AUDIO_BLOCK * 64);

    *valid = h >= -320 && h <= 320;
    h += CAL_OFF_NEUTRAL;
    return (int)(h < -32768 ? -32768 : h > 32767 ? 32767 : h);
}

int cal_gain_from(int64_t signed_frames_sum, int *valid)
{
    float a = (float)((double)signed_frames_sum / AUDIO_BLOCK);
    float g = -13107.2f / (a * (1.0f / 128.0f) * (1.0f / 16384.0f));

    *valid = g >= -18800.0f && g <= -17000.0f;
    return *valid ? (int)g : 0;
}

int cal_ch_offset(int64_t frames_sum, int *valid)
{
    *valid = frames_sum >= -4192LL * AUDIO_BLOCK && frames_sum <= 4192LL * AUDIO_BLOCK;
    return *valid ? (int)-floor_div(frames_sum, (int64_t)AUDIO_BLOCK * 64) : 0;
}

int32_t cal_mix_offset(int64_t level_sum, int *valid)
{
    *valid = level_sum >= -0x52000000LL && level_sum <= 0x52000000LL;
    return *valid ? (int32_t)floor_div(level_sum, 64) : 0;
}

int32_t cal_in_offset(int64_t level_sum, int *valid)
{
    int64_t o = -floor_div(level_sum, CAL_SAMPLES);

    *valid = o >= -31457280 && o <= 31457280;
    return *valid ? (int32_t)o : 0;
}

int cal_ch_responds(int up, int c)
{
    return up ? c >= 100 && c <= 500 : c >= -500 && c <= -100;
}

int cal_mix_responds(int up, int32_t reading)
{
    return up ? reading >= -85983232 && reading <= -2162688 : reading >= 2162688 && reading <= 85983232;
}

void cal_iter_init(struct cal_iter *it, int active)
{
    it->off = it->best_off = it->resid = 0;
    it->best_abs = INT64_MAX;
    it->valid = 0;
    it->done = !active;
    it->started = !active;
}

static void iter(struct cal_iter *it, int64_t resid, int64_t thr, int step_ok, int32_t step, int32_t lo, int32_t hi)
{
    int64_t a = resid < 0 ? -resid : resid;

    if (it->done) {
        return;
    }
    if (!it->started) {
        it->started = 1;
        it->valid = step_ok;
        it->resid = (int32_t)resid;
        if (!step_ok) {
            it->done = 1;
            return;
        }
    }
    if (a >= it->best_abs) {
        it->off = it->best_off;
        it->done = 1;
        return;
    }
    it->best_abs = a;
    it->best_off = it->off;
    it->resid = (int32_t)resid;
    if (a < thr || !step_ok || step == 0 || it->off + step < lo || it->off + step > hi) {
        it->done = 1;
        return;
    }
    it->off += step;
}

void cal_mix_iter(struct cal_iter *it, int64_t level_sum)
{
    int v;
    int32_t step = cal_mix_offset(level_sum, &v);

    iter(it, level_sum / CAL_SAMPLES, 107374, v, step, -CAL_MIX_TEST, CAL_MIX_TEST);
}

void cal_ch_iter(struct cal_iter *it, int64_t frames_sum)
{
    int v;
    int step = cal_ch_offset(frames_sum, &v);

    iter(it, frames_sum / (AUDIO_BLOCK * CAL_SAMPLES) * 32768, 4 * 32768, v, step, -66, 66);
}
