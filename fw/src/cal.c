#include "cal.h"

#include <string.h>
#include "ff.h"
#include "mmc.h"
#include "cv.h"
#include "rec.h"
#include "audio.h"
#include "engine.h"
#include "storage.h"
#include "samples.h"
#include "systime.h"

#define CAL_PATH    "0:cal.bin"
#define CAL_NEW     "0:calnew.bin"
#define CAL_BAK     "0:cal.bak"
#define CAL_TICK_US 30000u      /* stock samples once per UI tick, ~30 ms */

struct cal cal_live, cal_loaded;
const char *cal_source = "DEFAULT";
const char *cal_msg, *cal_msg2;
int cal_failed;
volatile int cal_mute;

static FATFS som;

static void defaults(struct cal *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < CAL_CV; i++) {
        c->cv_off[i] = CAL_OFF_NEUTRAL;
        c->cv_gain[i] = CAL_GAIN_DEFAULT;
    }
}

static int32_t le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, int32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)((uint32_t)v >> (8 * i));
    }
}

static void unpack(struct cal *c, const uint8_t *b)
{
    for (int i = 0; i < CAL_CV; i++) {
        c->cv_off[i] = (int16_t)(b[2 * i] | b[2 * i + 1] << 8);
        c->cv_gain[i] = (int16_t)(b[48 + 2 * i] | b[48 + 2 * i + 1] << 8);
    }
    for (int i = 0; i < 2; i++) {
        c->in_off[i] = le32(b + 96 + 4 * i);
        c->mix_off[i] = le32(b + 104 + 4 * i);
    }
    for (int i = 0; i < 8; i++) {
        c->ch_off[i] = (int8_t)b[112 + i];
    }
}

static void pack(const struct cal *c, uint8_t *b)
{
    for (int i = 0; i < CAL_CV; i++) {
        b[2 * i] = (uint8_t)c->cv_off[i];
        b[2 * i + 1] = (uint8_t)((uint16_t)c->cv_off[i] >> 8);
        b[48 + 2 * i] = (uint8_t)c->cv_gain[i];
        b[48 + 2 * i + 1] = (uint8_t)((uint16_t)c->cv_gain[i] >> 8);
    }
    for (int i = 0; i < 2; i++) {
        put32(b + 96 + 4 * i, c->in_off[i]);
        put32(b + 104 + 4 * i, c->mix_off[i]);
    }
    for (int i = 0; i < 8; i++) {
        b[112 + i] = (uint8_t)c->ch_off[i];
    }
}

static int som_mount(void)
{
    f_mount(0, "0:", 0);
    return f_mount(&som, "0:", 1) == FR_OK;
}

void cal_load(void)
{
    static uint8_t buf[CAL_BYTES];
    FIL f;
    UINT n = 0;

    defaults(&cal_loaded);
    cal_source = "DEFAULT (NO CAL.BIN)";
    if (som_mount() && f_open(&f, CAL_PATH, FA_READ) == FR_OK) {
        if (f_read(&f, buf, sizeof(buf), &n) == FR_OK && n == sizeof(buf)) {
            unpack(&cal_loaded, buf);
            cal_source = "CAL.BIN";
        } else {
            cal_source = "DEFAULT (BAD CAL.BIN)";
        }
        f_close(&f);
    }
    cal_live = cal_loaded;
}

int cal_dirty(void)
{
    return memcmp(&cal_live, &cal_loaded, sizeof(cal_live)) != 0;
}

void cal_discard(void)
{
    cal_capture_cancel();
    cal_live = cal_loaded;
}

/* ---- Saving. Boot card writes happen only through these, and only for
 * the three names. ---- */

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int allowed(const char *path)
{
    return same(path, CAL_PATH) || same(path, CAL_NEW) || same(path, CAL_BAK);
}

static FRESULT w_create(FIL *f, const char *path)
{
    FRESULT r;

    if (!allowed(path)) {
        return FR_DENIED;
    }
    mmc_som_writable = 1;
    r = f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS);
    mmc_som_writable = 0;
    return r;
}

/* f opened by w_create. */
static FRESULT w_write(FIL *f, const void *p, UINT n)
{
    UINT put = 0;
    FRESULT r;

    mmc_som_writable = 1;
    r = f_write(f, p, n, &put);
    if (r == FR_OK) {
        r = f_sync(f);
    }
    mmc_som_writable = 0;
    return r == FR_OK && put != n ? FR_DENIED : r;
}

static FRESULT w_close(FIL *f)
{
    FRESULT r;

    mmc_som_writable = 1;
    r = f_close(f);
    mmc_som_writable = 0;
    return r;
}

static FRESULT w_unlink(const char *path)
{
    FRESULT r;

    if (!allowed(path)) {
        return FR_DENIED;
    }
    mmc_som_writable = 1;
    r = f_unlink(path);
    mmc_som_writable = 0;
    return r;
}

static FRESULT w_rename(const char *from, const char *to)
{
    FRESULT r;

    if (!allowed(from) || !allowed(to)) {
        return FR_DENIED;
    }
    mmc_som_writable = 1;
    r = f_rename(from, to);
    mmc_som_writable = 0;
    return r;
}

#define COPY_MAX 4096u

static uint8_t old_bytes[COPY_MAX], check[COPY_MAX];
static UINT old_n;

/* The whole file into dst (at most COPY_MAX bytes); 1 if read in full. */
static int read_all(const char *path, uint8_t *dst, UINT *n)
{
    FIL f;
    int ok;

    *n = 0;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        return 0;
    }
    ok = f_size(&f) <= COPY_MAX && f_read(&f, dst, COPY_MAX, n) == FR_OK && *n == f_size(&f);
    f_close(&f);
    return ok;
}

/* Writes n bytes as path, then reads them back from the card. */
static int write_verified(const char *path, const uint8_t *p, UINT n)
{
    FIL f;
    UINT got;
    FRESULT r = w_create(&f, path);

    if (r != FR_OK) {
        cal_msg = r == FR_WRITE_PROTECTED ? "BOOT CARD LOCKED" : "CAN'T WRITE BOOT CARD";
        return 0;
    }
    r = w_write(&f, p, n);
    if (w_close(&f) != FR_OK || r != FR_OK) {
        cal_msg = r == FR_DENIED ? "BOOT CARD FULL" : "WRITE FAILED";
        return 0;
    }
    if (!som_mount() || !read_all(path, check, &got) || got != n || memcmp(check, p, n)) {
        cal_msg = "VERIFY FAILED";
        return 0;
    }
    return 1;
}

static int save(void);

int cal_save(void)
{
    int ok = save();

    cal_failed = !ok;
    return ok;
}

static int save(void)
{
    static uint8_t img[CAL_BYTES];
    int had_old;

    cal_msg = cal_msg2 = 0;
    if (storage_job_active() || samples_take_open() || rec_state() >= REC_RECORDING) {
        cal_msg = "BUSY";
        cal_msg2 = "CAL.BIN UNCHANGED";
        return 0;
    }
    if (!som_mount()) {
        cal_msg = "NO BOOT CARD";
        return 0;
    }
    pack(&cal_live, img);
    had_old = read_all(CAL_PATH, old_bytes, &old_n);
    if (!had_old) {
        FILINFO st;
        if (f_stat(CAL_PATH, &st) == FR_OK) {
            cal_msg = "CAN'T READ CAL.BIN";
            cal_msg2 = "CAL.BIN UNCHANGED";
            return 0;
        }
    }
    if (!write_verified(CAL_NEW, img, CAL_BYTES) || (had_old && !write_verified(CAL_BAK, old_bytes, old_n))) {
        w_unlink(CAL_NEW);
        cal_msg2 = "CAL.BIN UNCHANGED";
        return 0;
    }
    if (had_old && w_unlink(CAL_PATH) != FR_OK) {
        w_unlink(CAL_NEW);
        cal_msg = "CAN'T REMOVE CAL.BIN";
        cal_msg2 = som_mount() && f_stat(CAL_PATH, 0) == FR_OK ? "CAL.BIN UNCHANGED" : "CHECK THE BOOT CARD";
        return 0;
    }
    if (w_rename(CAL_NEW, CAL_PATH) != FR_OK) {
        const char *why = "CAN'T RENAME CALNEW.BIN";
        w_unlink(CAL_NEW);
        if (!had_old) {
            cal_msg2 = "NO CAL.BIN (AS BEFORE)";
        } else if (write_verified(CAL_PATH, old_bytes, old_n)) {
            cal_msg2 = "OLD CAL.BIN RESTORED";
        } else {
            cal_msg2 = "CHECK THE BOOT CARD";
        }
        cal_msg = why;
        return 0;
    }
    if (!som_mount() || !read_all(CAL_PATH, check, &old_n) || old_n != CAL_BYTES || memcmp(check, img, CAL_BYTES)) {
        cal_msg = "VERIFY FAILED";
        cal_msg2 = "CHECK THE BOOT CARD";
        return 0;
    }
    cal_loaded = cal_live;
    cal_source = "CAL.BIN";
    cal_msg = "SAVED TO CAL.BIN";
    cal_msg2 = had_old ? "OLD ONE IS CAL.BAK" : 0;
    return 1;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;

    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* ---- Live readings ---- */

static int page_on;
static int32_t cv_sum[CAL_CV];      /* block sums, stock order */
static int32_t in_lvl[2];
static uint32_t cv_epoch;

/* Stock index (g * 8 + n) to ours (n * 3 + g). */
static int ours(int i)
{
    return (i & 7) * 3 + i / 8;
}

/* Fresh block sums since the last call: 1 if both moved on. */
static int snapshot(void)
{
    uint32_t e, e2;
    int32_t t[CV_INPUTS];
    int fresh = 0;

    do {
        e = cv_sums_epoch;
        for (int i = 0; i < CV_INPUTS; i++) {
            t[i] = cv_sums[i];
        }
        e2 = cv_sums_epoch;
    } while (e != e2);
    if (e != cv_epoch) {
        cv_epoch = e;
        fresh = 1;
        for (int i = 0; i < CAL_CV; i++) {
            cv_sum[i] = t[ours(i)];
        }
    }
    do {
        e = rec_level_epoch;
        in_lvl[0] = rec_level[0];
        in_lvl[1] = rec_level[1];
        e2 = rec_level_epoch;
    } while (e != e2);
    return fresh;
}

void cal_page(int on)
{
    if (!on) {
        cal_capture_cancel();
    }
    page_on = on;
    cv_sums_on = on;
    rec_level_on = on;
}

int cal_page_open(void)
{
    return page_on;
}

int cal_cv_c(int i)
{
    return (int)floor_div(cv_sum[i], AUDIO_BLOCK);
}

int32_t cal_in_level(int lr)
{
    return in_lvl[lr];
}

/* ---- Capture ---- */

static int cap = CAP_NONE, kind, taken, settle, phase;
static uint32_t mask, ok, neg, pass[2], plus1;
static struct cal prev;
static int hushed;
static int64_t acc[CAL_CV], acc_in[2];
static uint32_t last_tick;
static struct cal_iter it_ch[8], it_mix[2];
static int out_pass;

int cal_capture_state(void) { return cap; }
uint32_t cal_capture_mask(void) { return mask; }
uint32_t cal_capture_ok(void) { return ok; }
uint32_t cal_capture_neg(void) { return neg; }
int cal_capture_progress(void) { return taken; }
int cal_out_pass(void) { return out_pass; }
int32_t cal_out_resid(int n) { return n < 8 ? it_ch[n].resid : it_mix[n - 8].resid; }

#define OUT_MASK 0x3ffu

/* The outputs and sample-input captures run with every voice and effect
 * stopped: nothing playing or modulated reaches the measured inputs. */
static void hush(int on)
{
    if (on && !hushed) {
        engine_hush();
    } else if (!on && hushed) {
        engine_unhush();
    }
    hushed = on;
}

static void set_pattern(int p)
{
    for (int n = 0; n < 8; n++) {
        cal_live.ch_off[n] = (int8_t)(((n & 1) == p) ? 127 : -127);
    }
    cal_live.mix_off[0] = p ? -CAL_MIX_TEST : CAL_MIX_TEST;
    cal_live.mix_off[1] = p ? CAL_MIX_TEST : -CAL_MIX_TEST;
}

static int in_win(int32_t v, int32_t lo, int32_t hi)
{
    return v >= lo && v <= hi;
}

/* The response to pattern p (output n drove +127 when (n & 1) == p). */
static uint32_t pattern_pass(int p)
{
    uint32_t r = 0;

    for (int n = 0; n < 8; n++) {
        r |= (uint32_t)cal_ch_responds((n & 1) == p, cal_cv_c(n)) << n;
    }
    for (int lr = 0; lr < 2; lr++) {
        r |= (uint32_t)cal_mix_responds(lr == 0 ? !p : p, in_lvl[lr]) << (8 + lr);
    }
    return r;
}

static void live_flags(void)
{
    ok = neg = 0;
    switch (cap) {
    case CAP_ZERO_WAIT:
        for (int i = 0; i < CAL_CV; i++) {
            ok |= (uint32_t)in_win(cal_cv_c(i), -640, 641) << i;
        }
        break;
    case CAP_GAIN1_WAIT:
        for (int i = 0; i < CAL_CV; i++) {
            ok |= (uint32_t)in_win(cal_cv_c(i), 22845, 25264) << i;
            neg |= (uint32_t)in_win(cal_cv_c(i), -25263, -22846) << i;
        }
        break;
    case CAP_GAIN2_WAIT:
        for (int i = 0; i < CAL_CV; i++) {
            int c = cal_cv_c(i);
            ok |= (uint32_t)((plus1 >> i & 1) ? in_win(c, -25264, -22845) : in_win(c, 22845, 25264)) << i;
        }
        break;
    case CAP_IN_WAIT:
        for (int lr = 0; lr < 2; lr++) {
            ok |= (uint32_t)in_win(in_lvl[lr], -31457280, 31457280) << lr;
        }
        break;
    }
    ok &= mask;
    neg &= mask;
}

void cal_capture(int k, uint32_t m)
{
    cal_capture_cancel();
    prev = cal_live;
    kind = k;
    taken = 0;
    ok = neg = 0;
    switch (k) {
    case CAPK_ZERO:
    case CAPK_GAIN:
        mask = m & ((1u << CAL_CV) - 1);
        for (int i = 0; i < CAL_CV; i++) {
            if (mask >> i & 1) {
                if (k == CAPK_ZERO) {
                    cal_live.cv_off[i] = CAL_OFF_NEUTRAL;
                }
                cal_live.cv_gain[i] = CAL_OFF_NEUTRAL;
            }
        }
        cap = k == CAPK_ZERO ? CAP_ZERO_WAIT : CAP_GAIN1_WAIT;
        break;
    case CAPK_OUT:
        mask = OUT_MASK;
        pass[0] = pass[1] = 0;
        phase = 0;
        hush(1);
        cal_mute = 1;
        set_pattern(0);
        cap = CAP_OUT_WAIT;
        break;
    case CAPK_IN:
        mask = 3;
        hush(1);
        cal_live.in_off[0] = cal_live.in_off[1] = 0;
        cap = CAP_IN_WAIT;
        break;
    }
    last_tick = time_us();
}

void cal_capture_cancel(void)
{
    if (cap != CAP_NONE) {
        if (cap != CAP_ZERO_DONE && cap != CAP_GAIN_DONE && cap != CAP_OUT_DONE && cap != CAP_IN_DONE) {
            cal_live = prev;
        }
        cap = CAP_NONE;
    }
    cal_mute = 0;
    hush(0);
}

static void measure(int next)
{
    cap = next;
    taken = 0;
    settle = 2;
    if (next != CAP_GAIN2_MEAS) {
        memset(acc, 0, sizeof(acc));
    }
    acc_in[0] = acc_in[1] = 0;
}

void cal_capture_next(void)
{
    switch (cap) {
    case CAP_ZERO_WAIT: measure(CAP_ZERO_MEAS); break;
    case CAP_GAIN1_WAIT:
        plus1 = ~neg;
        measure(CAP_GAIN1_MEAS);
        break;
    case CAP_GAIN2_WAIT: measure(CAP_GAIN2_MEAS); break;
    case CAP_OUT_WAIT:
        ok = pass[0] & pass[1];
        plus1 = ok;
        for (int n = 0; n < 8; n++) {
            cal_live.ch_off[n] = 0;
        }
        cal_live.mix_off[0] = cal_live.mix_off[1] = 0;
        for (int n = 0; n < 10; n++) {
            cal_iter_init(n < 8 ? &it_ch[n] : &it_mix[n - 8], plus1 >> n & 1);
        }
        out_pass = 1;
        measure(CAP_OUT_MEAS);
        break;
    case CAP_IN_WAIT: measure(CAP_IN_MEAS); break;
    case CAP_ZERO_DONE: case CAP_GAIN_DONE: case CAP_OUT_DONE: case CAP_IN_DONE:
        cap = CAP_NONE;
        break;
    }
}

static void finish(void)
{
    int v;

    ok = 0;
    switch (cap) {
    case CAP_ZERO_MEAS:
        for (int i = 0; i < CAL_CV; i++) {
            if (mask >> i & 1) {
                int o = cal_zero_offset(acc[i], &v);
                cal_live.cv_gain[i] = prev.cv_gain[i];
                cal_live.cv_off[i] = v ? (int16_t)o : prev.cv_off[i];
                ok |= (uint32_t)v << i;
            }
        }
        cap = CAP_ZERO_DONE;
        break;
    case CAP_GAIN2_MEAS:
        for (int i = 0; i < CAL_CV; i++) {
            if (mask >> i & 1) {
                int g = cal_gain_from(acc[i], &v);
                cal_live.cv_gain[i] = v ? (int16_t)g : prev.cv_gain[i];
                ok |= (uint32_t)v << i;
            }
        }
        cap = CAP_GAIN_DONE;
        break;
    case CAP_OUT_MEAS: {
        int all = 1;

        for (int n = 0; n < 10; n++) {
            struct cal_iter *it = n < 8 ? &it_ch[n] : &it_mix[n - 8];
            if (n < 8) {
                cal_ch_iter(it, acc[n]);
            } else {
                cal_mix_iter(it, acc_in[n - 8]);
            }
            all &= it->done;
        }
        if (!all && out_pass < CAL_OUT_PASSES) {
            out_pass++;
            for (int n = 0; n < 8; n++) {
                cal_live.ch_off[n] = (int8_t)it_ch[n].off;
            }
            for (int lr = 0; lr < 2; lr++) {
                cal_live.mix_off[lr] = it_mix[lr].off;
            }
            measure(CAP_OUT_MEAS);
            break;
        }
        for (int n = 0; n < 8; n++) {
            v = it_ch[n].valid;
            cal_live.ch_off[n] = v ? (int8_t)it_ch[n].off : prev.ch_off[n];
            ok |= (uint32_t)v << n;
        }
        for (int lr = 0; lr < 2; lr++) {
            v = it_mix[lr].valid;
            cal_live.mix_off[lr] = v ? it_mix[lr].off : prev.mix_off[lr];
            ok |= (uint32_t)v << (8 + lr);
        }
        cal_mute = 0;
        hush(0);
        cap = CAP_OUT_DONE;
        break;
    }
    case CAP_IN_MEAS:
        for (int lr = 0; lr < 2; lr++) {
            int32_t o = cal_in_offset(acc_in[lr], &v);
            cal_live.in_off[lr] = v ? o : prev.in_off[lr];
            ok |= (uint32_t)v << lr;
        }
        hush(0);
        cap = CAP_IN_DONE;
        break;
    }
}

static void sample(void)
{
    for (int i = 0; i < CAL_CV; i++) {
        int64_t c = cv_sum[i];
        if (cap == CAP_GAIN1_MEAS) {
            c = (plus1 >> i & 1) ? c : -c;
        } else if (cap == CAP_GAIN2_MEAS) {
            c = (plus1 >> i & 1) ? -c : c;
        }
        acc[i] += c;
    }
    acc_in[0] += in_lvl[0];
    acc_in[1] += in_lvl[1];
    if (++taken < CAL_SAMPLES) {
        return;
    }
    if (cap == CAP_GAIN1_MEAS) {
        cap = CAP_GAIN2_WAIT;
        taken = 0;
        return;
    }
    finish();
}

void cal_poll(void)
{
    uint32_t now = time_us();

    if (!page_on || now - last_tick < CAL_TICK_US) {
        return;
    }
    last_tick = now;
    if (!snapshot()) {
        return;
    }
    switch (cap) {
    case CAP_ZERO_MEAS: case CAP_GAIN1_MEAS: case CAP_GAIN2_MEAS: case CAP_OUT_MEAS: case CAP_IN_MEAS:
        if (settle) {
            settle--;
        } else {
            sample();
        }
        break;
    case CAP_OUT_WAIT:
        pass[phase] = pattern_pass(phase);
        phase ^= 1;
        set_pattern(phase);
        ok = pass[0] & pass[1];
        break;
    case CAP_ZERO_WAIT: case CAP_GAIN1_WAIT: case CAP_GAIN2_WAIT: case CAP_IN_WAIT:
        live_flags();
        break;
    }
}
