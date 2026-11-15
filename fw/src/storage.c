#include "storage.h"
#include "diag.h"
#include "engine.h"
#include "ff.h"
#include "fx.h"
#include "mmc.h"
#include "oled.h"
#include "ops.h"
#include "rec.h"
#include "samples.h"
#include "wav.h"
#include "systime.h"
#include "update.h"
#include "yml.h"

#include <string.h>

#define ARENA_SIZE      (16u << 20)
#define FILE_MAX        (128u << 10)
#define MAX_FOLDERS     512
#define NAME_POOL       (48u << 10)
#define PREFS_PATH      "1:A8M-PREFS.YML"
#define PREFS_DELAY_US  3000000u
#define POLL_BUDGET_US  4000u
#define SAMPLE_BUDGET_US 6000u
#define SAMPLE_RECHECK_US 500000u
#define TMP_NAME        "a8m-save.tmp"
#define FPREFS_NAME     "folderprefs.yml"
#define SSETUP_PATH     "1:A8M-SSETUP.YML"

struct slot {
    uint8_t state;
    uint8_t on_card;        /* saved = the card's file */
    uint8_t changed;        /* p != saved; not kept up to date for the live slot */
    uint8_t keep_gone;      /* kept lines didn't fit the arena: re-read the file */
    uint8_t force;          /* unsaved although p == saved (a sample renamed or erased) */
    const struct yml_keep *keep;
    struct preset p, saved;
};

static struct slot slots[PRESET_SLOTS + 1];
/* Kept lines of the folder's presets, bump-allocated, reset per folder.
 * Never written after allocation, so slots may share a record. */
static uint8_t arena[ARENA_SIZE] __attribute__((section(".noinit.arena"), aligned(8)));
static uint32_t arena_used;
static char filebuf[FILE_MAX] __attribute__((aligned(8)));
static uint32_t scratch_mem[(YML_KEEP_SIZE(YML_KEEP_FULL) + 3) / 4];
#define scratch ((struct yml_keep *)scratch_mem)

static FATFS fs;
static int mounted;
const char *storage_error;

static char folder[FF_LFN_BUF + 1];
static char path[FF_LFN_BUF + 32];
static int total, pending, bg_next, bg_stalled, changed_others;
static uint32_t load_t0, load_us;
/* Sample loading: the sample in progress, progress bytes, idle rechecks. */
static int smp_cur = -1, smp_idle, smp_demand = -1;
static uint32_t smp_recheck_at, smp_need, smp_done, smp_load_us;

static char names[NAME_POOL];
static uint16_t name_at[MAX_FOLDERS];
static int nfolders;

int pref_browse_preview = 1;
int pref_auto_audition = 1;
int pref_load_clean = 0;
int pref_load_warn = WARN_ALWAYS;
int pref_gate_delay;
int pref_brightness = 8;
int pref_saver = 2;
uint32_t pref_hang_resets;
static int fprefs_due;
static uint32_t fprefs_at;
uint32_t storage_cv_latency_us;
static char pref_folder[FF_LFN_BUF + 1];
/* What a cancelled folder load goes back to (stock: the previous folder and
 * preset); "" when the load in progress can't be cancelled. */
static char prev_folder[FF_LFN_BUF + 1];
static int prev_preset;
static int pref_preset, prefs_due;
static uint32_t prefs_at;
static char prefs_text[FF_LFN_BUF + 256], prefs_last[sizeof(prefs_text)];

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int icmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static void scopy(char *d, const char *s, size_t cap)
{
    size_t i = 0;

    for (; s[i] && i + 1 < cap; i++) {
        d[i] = s[i];
    }
    d[i] = 0;
}

static char *put3(char *o, int n)
{
    o[0] = (char)('0' + n / 100);
    o[1] = (char)('0' + n / 10 % 10);
    o[2] = (char)('0' + n % 10);
    return o + 3;
}

static void put_dec(char *o, int n)
{
    if (n >= 10) {
        *o++ = (char)('0' + n / 10);
    }
    *o++ = (char)('0' + n % 10);
    *o = 0;
}

/* "1:<folder>/<file>"; file = prstNNN.yml when name is 0. */
static const char *folder_path(int n, const char *name)
{
    char *o = path;

    memcpy(o, "1:", 2);
    o += 2;
    scopy(o, folder, sizeof(folder));
    o += slen(o);
    *o++ = '/';
    if (name) {
        scopy(o, name, 16);
    } else {
        memcpy(o, "prst", 4);
        memcpy(put3(o + 4, n), ".yml", 5);
    }
    return path;
}

static int is_wav(const char *s)
{
    size_t n = slen(s);

    return n > 4 && s[n - 4] == '.' && lower(s[n - 3]) == 'w' && lower(s[n - 2]) == 'a' &&
           lower(s[n - 1]) == 'v';
}

/* prstNNN.yml (any case) -> NNN, else 0. */
static int prst_number(const char *s)
{
    static const char pre[] = "prst", ext[] = ".yml";
    int n = 0;

    if (slen(s) != 11) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        if (lower(s[i]) != pre[i] || lower(s[7 + i]) != ext[i]) {
            return 0;
        }
    }
    for (int i = 4; i < 7; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return 0;
        }
        n = n * 10 + s[i] - '0';
    }
    return n;
}

static void set_live(const struct preset *p)
{
    uint32_t f;

    __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(f) :: "memory");
    preset = *p;
    __asm__ volatile("msr cpsr_c, %0" :: "r"(f) : "memory");
}

/* A preset load starts clean: the outputs fade out, every voice stops and
 * every effect's memory clears before the preset goes live. Carry (stock)
 * keeps what is sounding playing under the new preset. */
static void load_live(const struct preset *p)
{
    if (!pref_load_clean) {
        set_live(p);
        return;
    }
    engine_hush();
    set_live(p);
    fx_poll();
    fx_flush();
    engine_unhush();
}

static int differs(const struct slot *s, const struct preset *p)
{
    return s->force || memcmp(p, &s->saved, sizeof(*p)) != 0;
}

static int live_changed(void)
{
    return differs(&slots[preset_number], &preset);
}

/* A slot holding a fresh INIT preset that isn't on the card. */
static void slot_new(struct slot *s)
{
    preset_init(&s->saved);
    s->p = s->saved;
    s->state = SLOT_READY;
    s->on_card = 0;
    s->changed = 0;
    s->keep = 0;
    s->keep_gone = 0;
    s->force = 0;
}

#define E_TOO_BIG   (-1)

/* FatFs result, or E_TOO_BIG. */
static int read_file(const char *p, UINT *len)
{
    FIL f;
    FRESULT r = f_open(&f, p, FA_READ);

    *len = 0;
    if (r != FR_OK) {
        return r;
    }
    if (f_size(&f) > FILE_MAX) {
        f_close(&f);
        return E_TOO_BIG;
    }
    r = f_read(&f, filebuf, FILE_MAX, len);
    f_close(&f);
    return r;
}

/* 1 loaded, 0 no preset there (slot now empty), -1 card error (still pending). */
static int load_slot(int n)
{
    struct slot *s = &slots[n];
    UINT len;
    int r = read_file(folder_path(n, 0), &len);

    if (r == FR_OK && yml_parse(filebuf, len, &s->saved, scratch) == YML_OK) {
        static struct yml_samples names;
        uint32_t size = (uint32_t)YML_KEEP_SIZE(scratch->used);
        uint32_t need = (size + 7u) & ~7u;

        yml_samples(scratch, &names);
        for (int c = 0; c < CHANNELS; c++) {
            for (int z = 0; z < YML_ZONES; z++) {
                s->saved.ch[c].s.zone[z].smp =
                    (uint16_t)(names.name[c][z][0] ? samples_ref(names.name[c][z]) + 1 : 0);
            }
        }
        smp_idle = 0;

        s->keep = 0;
        s->keep_gone = 0;
        if (need <= ARENA_SIZE - arena_used) {
            struct yml_keep *k = (struct yml_keep *)(arena + arena_used);
            memcpy(k, scratch, size);
            k->cap = scratch->used;
            arena_used += need;
            s->keep = k;
        } else {
            s->keep_gone = 1;
        }
        s->saved.name[PRESET_NAME_LEN] = 0;
        s->p = s->saved;
        s->state = SLOT_READY;
        s->on_card = 1;
        s->changed = 0;
        s->force = 0;
        pending--;
        return 1;
    }
    if (r == FR_OK || r == FR_NO_FILE || r == E_TOO_BIG) {
        s->state = SLOT_EMPTY;
        pending--;
        return 0;
    }
    storage_error = mmc_error ? mmc_error : "CARD READ FAILED";
    return -1;
}

/* Kept lines to write slot n with: from the arena, or re-read from its
 * file when they didn't fit. 0 if they can't be had. */
static int keep_for(int n, const struct yml_keep **k)
{
    static struct preset tmp;
    UINT len;

    *k = slots[n].keep;
    if (!slots[n].keep_gone) {
        return 1;
    }
    if (read_file(folder_path(n, 0), &len) != FR_OK) {
        storage_error = "CAN'T RE-READ PRESET";
        return 0;
    }
    yml_parse(filebuf, len, &tmp, scratch);
    *k = scratch;
    return 1;
}

/* Through a temporary file, so a failed write leaves the old file. */
static int write_slot(int n, const struct preset *p, const struct yml_keep *k)
{
    char tmp[sizeof(path)];
    int len = yml_write(p, k, n, filebuf, FILE_MAX);
    FIL f;
    UINT w = 0;
    FRESULT r;

    if (len < 0) {
        storage_error = "PRESET TOO BIG";
        return 0;
    }
    scopy(tmp, folder_path(0, TMP_NAME), sizeof(tmp));
    r = f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS);
    if (r == FR_OK) {
        r = f_write(&f, filebuf, (UINT)len, &w);
        FRESULT c = f_close(&f);
        if (r == FR_OK) {
            r = c;
        }
        if (r == FR_OK && w != (UINT)len) {
            r = FR_DENIED;
        }
        if (r != FR_OK) {
            f_unlink(tmp);
        }
    }
    if (r == FR_OK) {
        r = f_unlink(folder_path(n, 0));
        if (r == FR_NO_FILE) {
            r = FR_OK;
        }
    }
    if (r == FR_OK) {
        r = f_rename(tmp, folder_path(n, 0));
    }
    if (r != FR_OK) {
        storage_error = mmc_error ? mmc_error : "CARD WRITE FAILED";
        return 0;
    }
    return 1;
}

static int can_write(void)
{
    if (!mounted) {
        storage_error = "NO CARD";
        return 0;
    }
    if (!folder[0]) {
        storage_error = "NO FOLDER";
        return 0;
    }
    return 1;
}

/* ---- Prefs ---- */

static int key_is(const char *k, size_t n, const char *name)
{
    size_t i = 0;

    for (size_t j = 0; j < n; j++) {
        if (k[j] == ' ' || k[j] == '\t') {
            continue;
        }
        if (!name[i] || lower(k[j]) != lower(name[i])) {
            return 0;
        }
        i++;
    }
    return !name[i];
}

static int val_is(const char *v, size_t n, const char *word)
{
    size_t i = 0;

    for (; i < n && word[i]; i++) {
        if (lower(v[i]) != lower(word[i])) {
            return 0;
        }
    }
    return i == n && !word[i];
}

static void prefs_line(const char *k, size_t kn, const char *v, size_t vn)
{
    if (key_is(k, kn, "LastFolder")) {
        if (vn < sizeof(pref_folder)) {
            memcpy(pref_folder, v, vn);
            pref_folder[vn] = 0;
        }
    } else if (key_is(k, kn, "LastPreset")) {
        int n = 0;
        for (size_t i = 0; i < vn && v[i] >= '0' && v[i] <= '9' && n < 10000; i++) {
            n = n * 10 + v[i] - '0';
        }
        pref_preset = n;
    } else if (key_is(k, kn, "BrowsePreview")) {
        if (val_is(v, vn, "off") || val_is(v, vn, "0") || val_is(v, vn, "no")) {
            pref_browse_preview = 0;
        } else if (val_is(v, vn, "on") || val_is(v, vn, "1") || val_is(v, vn, "yes")) {
            pref_browse_preview = 1;
        }
    } else if (key_is(k, kn, "AutoAudition")) {
        if (val_is(v, vn, "off") || val_is(v, vn, "0") || val_is(v, vn, "no")) {
            pref_auto_audition = 0;
        } else if (val_is(v, vn, "on") || val_is(v, vn, "1") || val_is(v, vn, "yes")) {
            pref_auto_audition = 1;
        }
    } else if (key_is(k, kn, "PresetLoad")) {
        if (val_is(v, vn, "carry")) {
            pref_load_clean = 0;
        } else if (val_is(v, vn, "clean")) {
            pref_load_clean = 1;
        }
    } else if (key_is(k, kn, "GateDelay")) {
        int n = 0;
        size_t i = 0;
        for (; i < vn && v[i] >= '0' && v[i] <= '9' && n <= 50; i++) {
            n = n * 10 + v[i] - '0';
        }
        if (i == vn && vn && n <= 50) {
            pref_gate_delay = n;
        }
    } else if (key_is(k, kn, "Brightness")) {
        int n = 0;
        size_t i = 0;
        for (; i < vn && v[i] >= '0' && v[i] <= '9' && n <= 11; i++) {
            n = n * 10 + v[i] - '0';
        }
        if (i == vn && n >= 1 && n <= 11) {
            pref_brightness = n;
        }
    } else if (key_is(k, kn, "ScreenSaver")) {
        int n = 0;
        size_t i = 0;
        for (; i < vn && v[i] >= '0' && v[i] <= '9' && n <= 60; i++) {
            n = n * 10 + v[i] - '0';
        }
        if (val_is(v, vn, "never")) {
            pref_saver = 0;
        } else if (i == vn && n >= 2 && n <= 60) {
            pref_saver = n;
        }
    } else if (key_is(k, kn, "HangResets")) {
        uint32_t n = 0;
        for (size_t i = 0; i < vn && v[i] >= '0' && v[i] <= '9' && n < 100000000u; i++) {
            n = n * 10u + (uint32_t)(v[i] - '0');
        }
        pref_hang_resets = n;
    } else if (key_is(k, kn, "LoadFolderWarn")) {
        if (val_is(v, vn, "never")) {
            pref_load_warn = WARN_NEVER;
        } else if (val_is(v, vn, "dirty")) {
            pref_load_warn = WARN_DIRTY;
        } else if (val_is(v, vn, "always")) {
            pref_load_warn = WARN_ALWAYS;
        }
    }
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Calls fn for each "key : value" line of filebuf[0..len). */
static void parse_lines(UINT len, void (*fn)(const char *k, size_t kn, const char *v, size_t vn))
{
    size_t pos = 0;

    while (pos < len) {
        size_t a = pos, e, colon;

        while (pos < len && filebuf[pos] != '\n') {
            pos++;
        }
        e = pos++;
        for (colon = a; colon < e && filebuf[colon] != ':'; colon++) {
        }
        if (colon == e) {
            continue;
        }
        size_t vb = colon + 1, ve = e, ke = colon;
        while (vb < ve && is_ws(filebuf[vb])) {
            vb++;
        }
        while (ve > vb && is_ws(filebuf[ve - 1])) {
            ve--;
        }
        while (ke > a && is_ws(filebuf[ke - 1])) {
            ke--;
        }
        fn(filebuf + a, ke - a, filebuf + vb, ve - vb);
    }
}

static void prefs_read(void)
{
    UINT len;

    if (read_file(PREFS_PATH, &len) == FR_OK) {
        parse_lines(len, prefs_line);
    }
    engine_set_gate_delay(pref_gate_delay);
    oled_set_brightness(pref_brightness);
}

/* ---- Folder prefs: <folder>/folderprefs.yml, stock's file and keys ---- */

static const char *const fprefs_keys[PCV_COUNT] = { "IncPresetCV", "DecPresetCV", "ResetPresetCV" };

static void fprefs_line(const char *k, size_t kn, const char *v, size_t vn)
{
    for (int i = 0; i < PCV_COUNT; i++) {
        if (key_is(k, kn, fprefs_keys[i]) && vn == 2 && v[0] >= '1' && v[0] <= '8' &&
            lower(v[1]) >= 'a' && lower(v[1]) <= 'c') {
            engine_preset_cv_in[i] = (uint8_t)(1 + (v[0] - '1') * 3 + (lower(v[1]) - 'a'));
        }
    }
}

static void fprefs_read(void)
{
    UINT len;

    for (int i = 0; i < PCV_COUNT; i++) {
        engine_preset_cv_in[i] = 0;
    }
    fprefs_due = 0;
    if (read_file(folder_path(0, FPREFS_NAME), &len) == FR_OK) {
        parse_lines(len, fprefs_line);
    }
}

static void fprefs_write(void)
{
    char text[128] = "# Folder Preferences\n";
    size_t n = slen(text);
    FIL f;
    UINT w = 0;

    if (!can_write()) {
        return;
    }
    for (int i = 0; i < PCV_COUNT; i++) {
        int in = engine_preset_cv_in[i];
        if (in) {
            scopy(text + n, "  ", sizeof(text) - n);
            scopy(text + n + 2, fprefs_keys[i], sizeof(text) - n - 2);
            n += slen(text + n);
            text[n++] = ' ';
            text[n++] = ':';
            text[n++] = ' ';
            text[n++] = (char)('1' + (in - 1) / 3);
            text[n++] = (char)('A' + (in - 1) % 3);
            text[n++] = '\n';
            text[n] = 0;
        }
    }
    if (f_open(&f, folder_path(0, FPREFS_NAME), FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, text, (UINT)n, &w);
        f_close(&f);
    }
}

void storage_folder_cv_changed(void)
{
    fprefs_due = 1;
    fprefs_at = time_us() + PREFS_DELAY_US;
}

static void prefs_cat(const char *s)
{
    size_t n = slen(prefs_text);

    scopy(prefs_text + n, s, sizeof(prefs_text) - n);
}

static void prefs_write(void)
{
    static const char *const warn[] = { "Always", "Dirty", "Never" };
    char num[4] = { 0 };
    FIL f;
    UINT w = 0;

    prefs_text[0] = 0;
    prefs_cat("LastFolder : ");
    prefs_cat(folder);
    prefs_cat("\nLastPreset : ");
    put3(num, preset_number);
    prefs_cat(num);
    prefs_cat("\nBrowsePreview : ");
    prefs_cat(pref_browse_preview ? "On" : "Off");
    prefs_cat("\nAutoAudition : ");
    prefs_cat(pref_auto_audition ? "On" : "Off");
    prefs_cat("\nPresetLoad : ");
    prefs_cat(pref_load_clean ? "Clean" : "Carry");
    prefs_cat("\nGateDelay : ");
    {
        char gd[4];
        put_dec(gd, pref_gate_delay);
        prefs_cat(gd);
    }
    prefs_cat("\nBrightness : ");
    put_dec(num, pref_brightness);
    prefs_cat(num);
    prefs_cat("\nScreenSaver : ");
    if (pref_saver) {
        put_dec(num, pref_saver);
        prefs_cat(num);
    } else {
        prefs_cat("Never");
    }
    prefs_cat("\nLoadFolderWarn : ");
    prefs_cat(warn[pref_load_warn]);
    prefs_cat("\n");
    if (pref_hang_resets) {
        char d[12];
        int n = 0;
        uint32_t v = pref_hang_resets;
        char r[12];
        do {
            d[n++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v);
        for (int i = 0; i < n; i++) {
            r[i] = d[n - 1 - i];
        }
        r[n] = 0;
        prefs_cat("HangResets : ");
        prefs_cat(r);
        prefs_cat("\n");
    }
    if (!memcmp(prefs_text, prefs_last, sizeof(prefs_text))) {
        return;
    }
    if (f_open(&f, PREFS_PATH, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, prefs_text, (UINT)slen(prefs_text), &w);
        if (f_close(&f) == FR_OK && w == slen(prefs_text)) {
            memcpy(prefs_last, prefs_text, sizeof(prefs_text));
        }
    }
}

void storage_prefs_changed(void)
{
    prefs_due = 1;
    prefs_at = time_us() + PREFS_DELAY_US;
}

void storage_count_hang(void)
{
    pref_hang_resets++;
    storage_prefs_changed();
}

/* ---- Sampling setup: 1:A8M-SSETUP.YML (src/ssetup.c) ---- */

static int ssetup_due;
static uint32_t ssetup_at;
static char ssetup_last[512];

static void ssetup_read(void)
{
    UINT len;

    if (read_file(SSETUP_PATH, &len) == FR_OK) {
        ssetup_parse((const char *)filebuf, len, &rec_setup);
    }
}

static void ssetup_write(void)
{
    char text[sizeof(ssetup_last)];
    size_t n = ssetup_text(&rec_setup, text, sizeof(text));
    FIL f;
    UINT w = 0;

    if (!n || !memcmp(text, ssetup_last, n + 1)) {
        return;
    }
    if (f_open(&f, SSETUP_PATH, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_write(&f, text, (UINT)n, &w);
        if (f_close(&f) == FR_OK && w == n) {
            memcpy(ssetup_last, text, n + 1);
        }
    }
}

void storage_ssetup_changed(void)
{
    ssetup_due = 1;
    ssetup_at = time_us() + PREFS_DELAY_US;
}

/* ---- Card and folders ---- */

static int mount(void)
{
    if (!mmc_init(MMC_FRONT)) {
        storage_error = "NO CARD";
        return 0;
    }
    if (f_mount(&fs, "1:", 1) != FR_OK) {
        storage_error = mmc_error ? mmc_error : "NO FAT CARD";
        return 0;
    }
    mounted = 1;
    bg_stalled = 0;
    storage_error = 0;
    return 1;
}

int storage_refresh(void)
{
    if (mounted) {
        if (mmc_present(MMC_FRONT)) {
            return 0;
        }
        f_mount(0, "1:", 0);
        mounted = 0;
        nfolders = 0;
        storage_error = "NO CARD";
    }
    return mount();
}

int storage_card(void)
{
    return mounted;
}

int storage_append(const char *path, const char *text, unsigned n)
{
    FIL f;
    UINT w = 0;
    FRESULT r;

    if (!mounted) {
        storage_error = "NO CARD";
        return 0;
    }
    if (storage_job_active() || update_busy()) {
        storage_error = "BUSY";
        return -1;
    }
    r = f_open(&f, path, FA_WRITE | FA_OPEN_APPEND);
    if (r == FR_OK) {
        r = f_write(&f, text, (UINT)n, &w);
        FRESULT c = f_close(&f);
        r = r == FR_OK ? c : r;
    }
    if (r != FR_OK || w != n) {
        storage_error = "CARD WRITE FAILED";
        return 0;
    }
    return 1;
}

int storage_scan(void)
{
    DIR d;
    FILINFO fi;
    size_t used = 0;

    nfolders = 0;
    if (!mounted || f_opendir(&d, "1:") != FR_OK) {
        return 0;
    }
    while (nfolders < MAX_FOLDERS && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t n = slen(fi.fname) + 1;

        if (!(fi.fattrib & AM_DIR) || (fi.fattrib & (AM_HID | AM_SYS)) || fi.fname[0] == '.' ||
            !icmp(fi.fname, "A8M") || used + n > NAME_POOL) {
            continue;
        }
        memcpy(names + used, fi.fname, n);
        name_at[nfolders++] = (uint16_t)used;
        used += n;
    }
    f_closedir(&d);
    for (int i = 1; i < nfolders; i++) {
        uint16_t v = name_at[i];
        int j = i;
        for (; j > 0 && icmp(names + name_at[j - 1], names + v) > 0; j--) {
            name_at[j] = name_at[j - 1];
        }
        name_at[j] = v;
    }
    return nfolders;
}

/* ---- Folder utilities ---- */

#define JOB_CHUNK       (32u << 10)
#define JOB_BUDGET_US   6000u
#define JOB_DEPTH       3
#define JOB_PATH        1100
#define REN_TMP         "1:A8M-REN.TMP"

static struct {
    int state;
    int final;                      /* state a cleanup ends in */
    int depth;
    int have_dir, have_in, have_out;
    uint32_t total, done;
    char dst[FF_LFN_BUF + 1];
    char dir[JOB_PATH];             /* delete: the directory being emptied */
    char src[FF_LFN_BUF + 1];
    DIR sd;
    FIL fi, fo;
} job;
static char jp1[JOB_PATH], jp2[JOB_PATH];

static const char *ferr(FRESULT r)
{
    switch (r) {
    case FR_EXIST: return "FILE EXISTS";
    case FR_DENIED: return "ACCESS DENIED";
    case FR_WRITE_PROTECTED: return "CARD LOCKED";
    case FR_NO_FILE:
    case FR_NO_PATH: return "NOT FOUND";
    case FR_INVALID_NAME: return "BAD NAME";
    default: return "CARD ERROR";
    }
}

static int in_use(const char *name)
{
    return folder[0] && !icmp(folder, name);
}

static int folder_ready(void)
{
    if (!mounted) {
        storage_error = "NO CARD";
        return 0;
    }
    if (storage_job_active() || samples_take_open()) {
        storage_error = "BUSY";
        return 0;
    }
    return 1;
}

/* A new folder name: legal in FAT, not hidden, unused (`self` = the name
 * being renamed, which may differ in case only). */
static int name_ok(const char *name, const char *self)
{
    FILINFO fi;
    size_t n = slen(name);
    FRESULT r;

    if (!n) {
        storage_error = "NAME EMPTY";
        return 0;
    }
    if (n > 100 || name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.' ||
        !icmp(name, "A8M")) {
        storage_error = "BAD NAME";
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)name[i] < 0x20 || strchr("\"*/:<>?\\|", name[i])) {
            storage_error = "BAD NAME";
            return 0;
        }
    }
    memcpy(jp1, "1:", 2);
    scopy(jp1 + 2, name, sizeof(jp1) - 2);
    r = f_stat(jp1, &fi);
    if (r == FR_OK && !(self && !icmp(self, name))) {
        storage_error = "FILE EXISTS";
        return 0;
    }
    if (r != FR_OK && r != FR_NO_FILE) {
        storage_error = ferr(r);
        return 0;
    }
    return 1;
}

int storage_name_ok(const char *name)
{
    return folder_ready() && name_ok(name, 0);
}

int storage_folder_in_use(const char *name)
{
    return in_use(name);
}

int storage_new_folder(const char *name)
{
    FRESULT r;

    if (!folder_ready() || !name_ok(name, 0)) {
        return 0;
    }
    memcpy(jp1, "1:", 2);
    scopy(jp1 + 2, name, sizeof(jp1) - 2);
    r = f_mkdir(jp1);
    storage_scan();
    if (r != FR_OK) {
        storage_error = ferr(r);
        return 0;
    }
    return 1;
}

int storage_rename_folder(const char *from, const char *to)
{
    FRESULT r;

    if (!folder_ready()) {
        return 0;
    }
    if (in_use(from)) {
        storage_error = "FOLDER IN USE";
        return 0;
    }
    if (!name_ok(to, from)) {
        return 0;
    }
    if (!memcmp(from, to, slen(from) + 1)) {
        return 1;
    }
    memcpy(jp1, "1:", 2);
    scopy(jp1 + 2, from, sizeof(jp1) - 2);
    memcpy(jp2, "1:", 2);
    scopy(jp2 + 2, to, sizeof(jp2) - 2);
    if (!icmp(from, to)) {
        r = f_rename(jp1, REN_TMP);
        if (r == FR_OK) {
            r = f_rename(REN_TMP, jp2);
            if (r != FR_OK) {
                f_rename(REN_TMP, jp1);
            }
        }
    } else {
        r = f_rename(jp1, jp2);
    }
    if (r == FR_OK && !icmp(pref_folder, from)) {
        scopy(pref_folder, to, sizeof(pref_folder));
    }
    storage_scan();
    if (r != FR_OK) {
        storage_error = ferr(r);
        return 0;
    }
    return 1;
}

static void job_end(int state)
{
    job.state = state;
    storage_scan();
}

static void job_fail(const char *err, int final)
{
    storage_error = err;
    if (job.have_out) {
        f_close(&job.fo);
    }
    if (job.have_in) {
        f_close(&job.fi);
    }
    if (job.have_dir) {
        f_closedir(&job.sd);
    }
    job.have_in = job.have_out = job.have_dir = 0;
    if (job.state == JOB_COPYING) {
        memcpy(job.dir, "1:", 2);
        scopy(job.dir + 2, job.dst, sizeof(job.dir) - 2);
        job.depth = 0;
        job.final = final;
        job.state = JOB_CLEANING;
    } else {
        job_end(JOB_FAILED);
    }
}

static void job_path(char *o, const char *dir, const char *name)
{
    size_t n;

    memcpy(o, "1:", 2);
    scopy(o + 2, dir, JOB_PATH - 2);
    n = slen(o);
    o[n++] = '/';
    scopy(o + n, name, JOB_PATH - n);
}

int storage_dup_start(const char *src, const char *dst)
{
    FILINFO fi;
    FRESULT r;
    uint32_t cl = (uint32_t)fs.csize * 512u, need = 0, files = 0;
    DWORD free_clst;
    FATFS *fp;

    if (!folder_ready() || !name_ok(dst, 0)) {
        return 0;
    }
    memcpy(jp1, "1:", 2);
    scopy(jp1 + 2, src, sizeof(jp1) - 2);
    job.total = 0;
    r = f_opendir(&job.sd, jp1);
    while (r == FR_OK && f_readdir(&job.sd, &fi) == FR_OK && fi.fname[0]) {
        if (!(fi.fattrib & AM_DIR) && icmp(fi.fname, TMP_NAME)) {
            job.total += fi.fsize;
            need += (fi.fsize + cl - 1) / cl;
            files++;
        }
    }
    if (r != FR_OK) {
        storage_error = "CAN'T OPEN FOLDER";
        return 0;
    }
    f_closedir(&job.sd);
    if (f_getfree("1:", &free_clst, &fp) != FR_OK || need + files * 96 / cl + 3 > free_clst) {
        storage_error = "CARD FULL";
        return 0;
    }
    memcpy(jp2, "1:", 2);
    scopy(jp2 + 2, dst, sizeof(jp2) - 2);
    r = f_mkdir(jp2);
    storage_scan();
    if (r != FR_OK || f_opendir(&job.sd, jp1) != FR_OK) {
        storage_error = ferr(r);
        if (r == FR_OK) {
            f_unlink(jp2);
            storage_scan();
            storage_error = "CAN'T OPEN FOLDER";
        }
        return 0;
    }
    scopy(job.src, src, sizeof(job.src));
    scopy(job.dst, dst, sizeof(job.dst));
    job.have_dir = 1;
    job.have_in = job.have_out = 0;
    job.done = 0;
    job.state = JOB_COPYING;
    storage_error = 0;
    return 1;
}

static void dup_step(void)
{
    UINT n = 0, w = 0;
    FRESULT r;

    if (!job.have_in) {
        FILINFO fi;

        do {
            r = f_readdir(&job.sd, &fi);
        } while (r == FR_OK && fi.fname[0] &&
                 ((fi.fattrib & AM_DIR) || !icmp(fi.fname, TMP_NAME)));
        if (r != FR_OK) {
            job_fail(ferr(r), JOB_FAILED);
            return;
        }
        if (!fi.fname[0]) {
            f_closedir(&job.sd);
            job.have_dir = 0;
            job_end(JOB_DONE);
            return;
        }
        job_path(jp1, job.src, fi.fname);
        job_path(jp2, job.dst, fi.fname);
        r = f_open(&job.fi, jp1, FA_READ);
        if (r != FR_OK) {
            job_fail(ferr(r), JOB_FAILED);
            return;
        }
        job.have_in = 1;
        r = f_open(&job.fo, jp2, FA_WRITE | FA_CREATE_NEW);
        if (r != FR_OK) {
            job_fail(ferr(r), JOB_FAILED);
            return;
        }
        job.have_out = 1;
        return;
    }
    r = f_read(&job.fi, filebuf, JOB_CHUNK, &n);
    if (r == FR_OK && n) {
        r = f_write(&job.fo, filebuf, n, &w);
        if (r == FR_OK && w != n) {
            job_fail("CARD FULL", JOB_FAILED);
            return;
        }
    }
    if (r != FR_OK) {
        job_fail(ferr(r), JOB_FAILED);
        return;
    }
    job.done += n;
    if (n < JOB_CHUNK) {
        job.have_in = 0;
        f_close(&job.fi);
        job.have_out = 0;
        r = f_close(&job.fo);
        if (r != FR_OK) {
            job_fail(ferr(r), JOB_FAILED);
        }
    }
}

int storage_delete_folder(const char *name)
{
    FILINFO fi;
    FRESULT r;

    if (!folder_ready()) {
        return 0;
    }
    /* Never let a bad name turn into the card root or a path. */
    if (!name[0] || name[0] == '.' || strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
        storage_error = "BAD NAME";
        return 0;
    }
    if (in_use(name)) {
        storage_error = "FOLDER IN USE";
        return 0;
    }
    memcpy(job.dir, "1:", 2);
    scopy(job.dir + 2, name, sizeof(job.dir) - 2);
    r = f_stat(job.dir, &fi);
    if (r != FR_OK || !(fi.fattrib & AM_DIR)) {
        storage_error = ferr(r == FR_OK ? FR_NO_FILE : r);
        return 0;
    }
    scopy(job.src, name, sizeof(job.src));
    job.depth = 0;
    job.have_dir = job.have_in = job.have_out = 0;
    job.state = JOB_DELETING;
    storage_error = 0;
    return 1;
}

/* Removes entries of job.dir until the budget ends; descends into
 * subfolders and removes each emptied one. */
static void del_step(uint32_t t0)
{
    static FILINFO fi;
    DIR d;
    FRESULT r = f_opendir(&d, job.dir);
    size_t base = slen(job.dir);

    if (r != FR_OK) {
        job_fail(ferr(r), JOB_FAILED);
        return;
    }
    for (;;) {
        r = f_readdir(&d, &fi);
        if (r != FR_OK) {
            f_closedir(&d);
            job_fail(ferr(r), JOB_FAILED);
            return;
        }
        if (!fi.fname[0]) {
            break;
        }
        if (base + 1 + slen(fi.fname) >= JOB_PATH) {
            f_closedir(&d);
            job_fail("PATH TOO LONG", JOB_FAILED);
            return;
        }
        job.dir[base] = '/';
        scopy(job.dir + base + 1, fi.fname, JOB_PATH - base - 1);
        if (fi.fattrib & AM_DIR) {
            if (job.depth + 1 >= JOB_DEPTH) {
                job.dir[base] = 0;
                f_closedir(&d);
                job_fail("TOO DEEP", JOB_FAILED);
                return;
            }
            job.depth++;
            f_closedir(&d);
            return;
        }
        if (fi.fattrib & AM_RDO) {
            f_chmod(job.dir, 0, AM_RDO);
        }
        r = f_unlink(job.dir);
        job.dir[base] = 0;
        if (r != FR_OK) {
            f_closedir(&d);
            job_fail(ferr(r), JOB_FAILED);
            return;
        }
        if (time_us() - t0 >= JOB_BUDGET_US) {
            f_closedir(&d);
            return;
        }
    }
    f_closedir(&d);
    f_chmod(job.dir, 0, AM_RDO);
    r = f_unlink(job.dir);
    if (r != FR_OK) {
        job_fail(ferr(r), JOB_FAILED);
        return;
    }
    if (job.depth) {
        size_t n = slen(job.dir);

        while (job.dir[n - 1] != '/') {
            n--;
        }
        job.dir[n - 1] = 0;
        job.depth--;
    } else {
        job_end(job.state == JOB_CLEANING ? job.final : JOB_DONE);
    }
}

/* ---- Sample import: files from anywhere on the card into the folder ---- */

#define IMP_MAX         64
#define IMP_DIR         200
#define IMP_CMP         (16u << 10)
#define BENT_MAX        16384

enum { IP_START, IP_CAND, IP_CMP, IP_COPY };
enum { IR_PENDING, IR_COPIED, IR_SAME, IR_FAILED };

struct imp {
    char dir[IMP_DIR];
    char name[SAMPLE_NAME_LEN + 1];
    char out[SAMPLE_NAME_LEN + 1];
    uint32_t size;
    uint8_t res;
};

static struct imp imps[IMP_MAX];
static int nimp;
static struct {
    int cur, phase, cand, ram, in_open, out_open;
    uint32_t pos, base;
    struct wav_info w;
    FIL fi, fo;
} ij;
static const char *imp_err;

void storage_import_clear(void)
{
    if (!storage_job_active()) {
        nimp = 0;
    }
}

int storage_import_add(const char *dir, const char *name)
{
    struct imp *e;

    for (int i = 0; i < nimp; i++) {
        if (!icmp(imps[i].dir, dir) && !icmp(imps[i].name, name)) {
            return i;
        }
    }
    if (nimp >= IMP_MAX || slen(dir) >= IMP_DIR || slen(name) > SAMPLE_NAME_LEN) {
        return -1;
    }
    e = &imps[nimp];
    memset(e, 0, sizeof(*e));
    scopy(e->dir, dir, sizeof(e->dir));
    scopy(e->name, name, sizeof(e->name));
    return nimp++;
}

/* "KICK.WAV" -> "KICK~1.WAV" for cand 1, 2, ...; cand 0 is the name itself. */
static void imp_cand(char *o, const char *name, int cand)
{
    size_t n = slen(name), base = n, ext;
    char num[4];

    if (!cand) {
        scopy(o, name, SAMPLE_NAME_LEN + 1);
        return;
    }
    while (base > 0 && name[base - 1] != '.') {
        base--;
    }
    base = base ? base - 1 : n;
    ext = n - base;
    put_dec(num, cand);
    if (base + 1 + slen(num) + ext > SAMPLE_NAME_LEN) {
        base = SAMPLE_NAME_LEN - 1 - slen(num) - ext;
    }
    memcpy(o, name, base);
    o[base] = '~';
    memcpy(o + base + 1, num, slen(num));
    memcpy(o + base + 1 + slen(num), name + n - ext, ext + 1);
}

static void imp_path(char *o, const char *dir, const char *name)
{
    size_t n;

    memcpy(o, "1:", 2);
    scopy(o + 2, dir, JOB_PATH - 2);
    n = slen(o);
    if (dir[0]) {
        o[n++] = '/';
    }
    scopy(o + n, name, JOB_PATH - n);
}

static uint32_t imp_read_cb(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    UINT got = 0;

    if (f_lseek((FIL *)ctx, off) != FR_OK || f_read((FIL *)ctx, buf, n, &got) != FR_OK) {
        return 0;
    }
    return got;
}

static void imp_close(void)
{
    if (ij.out_open) {
        f_close(&ij.fo);
    }
    if (ij.in_open) {
        f_close(&ij.fi);
    }
    ij.in_open = ij.out_open = 0;
}

/* Adds what was copied to the sample table and ends the job. */
static void imp_stop(const char *err, int state)
{
    imp_close();
    if (ij.phase == IP_COPY && err && ij.cur < nimp) {
        imp_path(jp1, folder, imps[ij.cur].out);
        f_unlink(jp1);
    }
    for (int i = 0; i < nimp; i++) {
        struct imp *e = &imps[i];
        int k;

        if (e->res != IR_COPIED) {
            continue;
        }
        k = samples_add(e->out, e->size);
        if (k >= 0 && samples[k].state == SMP_MISSING) {
            samples[k].state = SMP_FOUND;
            samples[k].file_size = e->size;
        }
    }
    smp_idle = 0;
    if (err) {
        storage_error = err;
    }
    job_end(state);
}

int storage_import_start(void)
{
    uint32_t total = 0;

    if (!folder_ready() || !folder[0]) {
        storage_error = storage_error ? storage_error : "NO FOLDER";
        return 0;
    }
    for (int i = 0; i < nimp; i++) {
        FILINFO fi;

        imps[i].res = IR_PENDING;
        imp_path(jp1, imps[i].dir, imps[i].name);
        if (f_stat(jp1, &fi) != FR_OK) {
            storage_error = "NOT FOUND";
            return 0;
        }
        imps[i].size = (uint32_t)fi.fsize;
        total += imps[i].size;
    }
    memset(&ij, 0, sizeof(ij));
    job.total = total;
    job.done = 0;
    job.state = JOB_IMPORTING;
    storage_error = 0;
    imp_err = 0;
    return 1;
}

int storage_import_result(int idx)
{
    if (idx < 0 || idx >= nimp || (imps[idx].res != IR_COPIED && imps[idx].res != IR_SAME)) {
        return -1;
    }
    return samples_find(imps[idx].out);
}

static void imp_next(int res)
{
    imps[ij.cur].res = (uint8_t)res;
    ij.base += imps[ij.cur].size;
    job.done = ij.base;
    imp_close();
    ij.cur++;
    ij.phase = IP_START;
    ij.cand = 0;
}

static void imp_fail_one(const char *err)
{
    imp_err = err;
    imp_next(IR_FAILED);
}

static void imp_step(void)
{
    struct imp *e;
    FILINFO fi;
    FRESULT r;
    UINT n = 0, w = 0;

    if (ij.cur >= nimp) {
        imp_stop(imp_err, imp_err ? JOB_FAILED : JOB_DONE);
        return;
    }
    e = &imps[ij.cur];
    switch (ij.phase) {
    case IP_START:
        if (!icmp(e->dir, folder)) {
            scopy(e->out, e->name, sizeof(e->out));
            imp_next(IR_SAME);
            return;
        }
        imp_path(jp1, e->dir, e->name);
        r = f_open(&ij.fi, jp1, FA_READ);
        if (r != FR_OK) {
            imp_fail_one(ferr(r));
            return;
        }
        ij.in_open = 1;
        if (wav_parse(imp_read_cb, &ij.fi, e->size, &ij.w)) {
            ij.w.frames = 0;
        }
        ij.phase = IP_CAND;
        return;
    case IP_CAND:
        if (ij.cand > 99) {
            imp_fail_one("TOO MANY COPIES");
            return;
        }
        imp_cand(e->out, e->name, ij.cand);
        imp_path(jp2, folder, e->out);
        r = f_stat(jp2, &fi);
        if (r == FR_NO_FILE) {
            r = f_open(&ij.fo, jp2, FA_WRITE | FA_CREATE_NEW);
            if (r != FR_OK) {
                imp_fail_one(ferr(r));
                return;
            }
            ij.out_open = 1;
            f_lseek(&ij.fi, 0);
            ij.pos = 0;
            ij.phase = IP_COPY;
            return;
        }
        if (r != FR_OK) {
            imp_fail_one(ferr(r));
            return;
        }
        if ((uint32_t)fi.fsize != e->size || (fi.fattrib & AM_DIR)) {
            ij.cand++;
            return;
        }
        {
            int k = samples_find(e->out);

            ij.ram = k >= 0 && samples[k].state == SMP_READY && !samples[k].is_float &&
                     ij.w.frames && !ij.w.is_float && samples[k].frames == ij.w.frames &&
                     samples[k].channels == ij.w.channels && samples[k].bits == ij.w.bits;
            if (!ij.ram && f_open(&ij.fo, jp2, FA_READ) != FR_OK) {
                imp_fail_one("CARD ERROR");
                return;
            }
            ij.out_open = !ij.ram;
        }
        ij.pos = ij.ram ? ij.w.data_off : 0;
        f_lseek(&ij.fi, ij.pos);
        ij.phase = IP_CMP;
        return;
    case IP_CMP: {
        uint32_t end = ij.ram ? ij.w.data_off + ij.w.frames * ij.w.block : e->size;
        uint32_t want = end - ij.pos < IMP_CMP ? end - ij.pos : IMP_CMP;
        uint8_t *a = (uint8_t *)filebuf, *b = a + IMP_CMP;
        int same;

        if (want) {
            r = f_read(&ij.fi, a, want, &n);
            if (r != FR_OK || n != want) {
                imp_fail_one("CARD ERROR");
                return;
            }
            if (ij.ram) {
                uint32_t blk = ij.w.block, f0 = (ij.pos - ij.w.data_off) / blk;

                samples_regen(samples_find(e->out), f0, want / blk, b);
            } else if (f_read(&ij.fo, b, want, &w) != FR_OK || w != want) {
                imp_fail_one("CARD ERROR");
                return;
            }
            same = !memcmp(a, b, want);
            ij.pos += want;
            if (!same) {
                if (ij.out_open) {
                    f_close(&ij.fo);
                    ij.out_open = 0;
                }
                f_lseek(&ij.fi, 0);
                ij.cand++;
                ij.phase = IP_CAND;
                return;
            }
        }
        if (ij.pos >= end) {
            imp_next(IR_SAME);
        }
        return;
    }
    case IP_COPY:
        r = f_read(&ij.fi, filebuf, JOB_CHUNK, &n);
        if (r == FR_OK && n) {
            r = f_write(&ij.fo, filebuf, n, &w);
            if (r == FR_OK && w != n) {
                imp_stop("CARD FULL", JOB_FAILED);
                return;
            }
        }
        if (r != FR_OK) {
            imp_stop(ferr(r), JOB_FAILED);
            return;
        }
        ij.pos += n;
        job.done = ij.base + ij.pos;
        if (n < JOB_CHUNK) {
            f_close(&ij.fi);
            ij.in_open = 0;
            ij.out_open = 0;
            r = f_close(&ij.fo);
            if (r != FR_OK) {
                imp_stop(ferr(r), JOB_FAILED);
                return;
            }
            imp_next(IR_COPIED);
        }
        return;
    }
}

/* ---- Truncation: a chunked job writing frames [from, to) of a sample as a
 * new <name>T00.wav (T01...), or over the sample's own file. ---- */

#define TRUNC_TMP       "a8m-trunc.tmp"

static struct {
    int smp, in_place, hdr_done, in_open, out_open;
    uint32_t from, pos, total;
    struct wav_info w;
    char out[SAMPLE_NAME_LEN + 1];
    FIL fi, fo;
} tj;
static int tj_result = -1;

/* "Snare1.wav" -> "Snare1T00.wav"; "Snare1T00.wav" -> "Snare1T01.wav". */
static int trunc_name(char *out, const char *src)
{
    char stem[SAMPLE_NAME_LEN + 1];
    size_t n = slen(src) - 4;
    int first = 0;

    memcpy(stem, src, n);
    if (n > 3 && lower(stem[n - 3]) == 't' && stem[n - 2] >= '0' && stem[n - 2] <= '9' && stem[n - 1] >= '0' &&
        stem[n - 1] <= '9') {
        char orig[SAMPLE_NAME_LEN + 1];

        memcpy(orig, stem, n - 3);
        memcpy(orig + n - 3, ".wav", 5);
        if (samples_find(orig) >= 0) {
            first = (stem[n - 2] - '0') * 10 + stem[n - 1] - '0' + 1;
            n -= 3;
        }
    }
    if (n > SAMPLE_NAME_LEN - 7) {
        n = SAMPLE_NAME_LEN - 7;
    }
    for (int k = first; k < 100; k++) {
        FILINFO fi;

        memcpy(out, stem, n);
        out[n] = 'T';
        out[n + 1] = (char)('0' + k / 10);
        out[n + 2] = (char)('0' + k % 10);
        memcpy(out + n + 3, ".wav", 5);
        imp_path(jp2, folder, out);
        if (samples_find(out) < 0 && f_stat(jp2, &fi) == FR_NO_FILE) {
            return 1;
        }
    }
    return 0;
}

static void trunc_close(void)
{
    if (tj.out_open) {
        f_close(&tj.fo);
    }
    if (tj.in_open) {
        f_close(&tj.fi);
    }
    tj.in_open = tj.out_open = 0;
}

static void trunc_stop(const char *err, int state)
{
    trunc_close();
    if (err) {
        imp_path(jp1, folder, tj.in_place ? TRUNC_TMP : tj.out);
        f_unlink(jp1);
        storage_error = err;
    }
    job_end(state);
}

int storage_trunc_start(int i, uint32_t from, uint32_t to, int in_place)
{
    struct sample *s;
    FRESULT r;

    if (!folder_ready() || !folder[0]) {
        storage_error = storage_error ? storage_error : "NO FOLDER";
        return 0;
    }
    if (!samples_valid(i) || i >= SAMPLES_MAX || samples[i].state < SMP_HEADER || samples[i].state > SMP_NOMEM ||
        from >= to || to > samples[i].frames) {
        storage_error = "BAD RANGE";
        return 0;
    }
    s = &samples[i];
    memset(&tj, 0, sizeof(tj));
    tj.smp = i;
    tj.in_place = in_place;
    tj.from = from;
    tj.w.rate = s->rate;
    tj.w.channels = s->channels;
    tj.w.bits = s->bits;
    tj.w.block = s->block;
    tj.w.is_float = s->is_float;
    tj.w.frames = to - from;
    tj.w.data_off = s->data_off;
    tj.total = tj.w.frames * s->block;
    if (!in_place && !trunc_name(tj.out, s->name)) {
        storage_error = "TOO MANY COPIES";
        return 0;
    }
    if (in_place) {
        scopy(tj.out, s->name, sizeof(tj.out));
    }
    imp_path(jp1, folder, s->name);
    if ((r = f_open(&tj.fi, jp1, FA_READ)) != FR_OK) {
        storage_error = ferr(r);
        return 0;
    }
    tj.in_open = 1;
    imp_path(jp2, folder, in_place ? TRUNC_TMP : tj.out);
    if ((r = f_open(&tj.fo, jp2, FA_WRITE | (in_place ? FA_CREATE_ALWAYS : FA_CREATE_NEW))) != FR_OK) {
        trunc_close();
        storage_error = ferr(r);
        return 0;
    }
    tj.out_open = 1;
    job.total = tj.total;
    job.done = 0;
    job.state = JOB_TRUNCATING;
    storage_error = 0;
    tj_result = -1;
    return 1;
}

int storage_trunc_result(void)
{
    return tj_result;
}

static void trunc_finish(void)
{
    uint32_t size;
    FRESULT r = FR_OK;
    UINT w;

    if (tj.total & 1u) {
        const uint8_t zero = 0;

        r = f_write(&tj.fo, &zero, 1, &w);
    }
    size = (uint32_t)f_size(&tj.fo);
    if (r == FR_OK) {
        r = f_close(&tj.fo);
        tj.out_open = 0;
    }
    if (r != FR_OK) {
        trunc_stop(ferr(r), JOB_FAILED);
        return;
    }
    trunc_close();
    imp_path(jp1, folder, tj.in_place ? tj.out : TRUNC_TMP);
    if (tj.in_place) {
        imp_path(jp2, folder, TRUNC_TMP);
        f_chmod(jp1, 0, AM_RDO);
        r = f_unlink(jp1);
        if (r == FR_OK) {
            r = f_rename(jp2, jp1);
        }
        if (r != FR_OK) {
            trunc_stop(ferr(r), JOB_FAILED);
            return;
        }
        engine_samples_stop();
        samples_refresh(tj.smp, size);
        if (smp_cur == tj.smp) {
            smp_cur = -1;
        }
        tj_result = tj.smp;
    } else {
        int k = samples_add(tj.out, size);

        if (k >= 0) {
            samples[k].state = SMP_FOUND;
            samples[k].file_size = size;
            samples_probe(k);
        }
        tj_result = k;
    }
    smp_idle = 0;
    job_end(JOB_DONE);
}

static void trunc_step(void)
{
    FRESULT r;
    UINT n = 0, w = 0;
    uint32_t want;

    if (!tj.hdr_done) {
        uint8_t h[WAV_HEADER_MAX];
        uint32_t hl = wav_header(h, &tj.w);

        r = f_write(&tj.fo, h, hl, &w);
        if (r == FR_OK && w == hl) {
            r = f_lseek(&tj.fi, tj.w.data_off + tj.from * tj.w.block);
        }
        if (r != FR_OK || w != hl) {
            trunc_stop(r != FR_OK ? ferr(r) : "CARD FULL", JOB_FAILED);
            return;
        }
        tj.hdr_done = 1;
        return;
    }
    if (tj.pos >= tj.total) {
        trunc_finish();
        return;
    }
    want = tj.total - tj.pos < JOB_CHUNK ? tj.total - tj.pos : JOB_CHUNK;
    r = f_read(&tj.fi, filebuf, want, &n);
    if (r != FR_OK || n != want) {
        trunc_stop(r != FR_OK ? ferr(r) : "WAV CUT SHORT", JOB_FAILED);
        return;
    }
    r = f_write(&tj.fo, filebuf, n, &w);
    if (r != FR_OK || w != n) {
        trunc_stop(r != FR_OK ? ferr(r) : "CARD FULL", JOB_FAILED);
        return;
    }
    tj.pos += n;
    job.done = tj.pos;
}

/* ---- A recorded sample written from memory: 24-bit PCM, as stock. ---- */

static struct {
    int smp, open;
    uint32_t pos;
    FIL fo;
} rw;

int storage_wav_exists(const char *name)
{
    FILINFO fi;

    if (!mounted || !folder[0]) {
        return 0;
    }
    imp_path(jp2, folder, name);
    return f_stat(jp2, &fi) == FR_OK;
}

static void recw_stop(const char *err)
{
    if (rw.open) {
        f_close(&rw.fo);
        rw.open = 0;
    }
    if (err) {
        imp_path(jp1, folder, samples[rw.smp].name);
        f_unlink(jp1);
        storage_error = err;
    } else {
        samples[rw.smp].ram_only = 0;
    }
    job_end(err ? JOB_FAILED : JOB_DONE);
}

int storage_rec_write(int i)
{
    struct sample *s = &samples[i];
    struct wav_info w = { 0 };
    uint8_t h[WAV_HEADER_MAX];
    uint32_t hl;
    UINT n = 0;
    FRESULT r;

    if (!folder_ready() || !folder[0]) {
        storage_error = storage_error ? storage_error : "NO FOLDER";
        return 0;
    }
    memset(&rw, 0, sizeof(rw));
    rw.smp = i;
    imp_path(jp1, folder, s->name);
    if ((r = f_open(&rw.fo, jp1, FA_WRITE | FA_CREATE_NEW)) != FR_OK) {
        storage_error = ferr(r);
        return 0;
    }
    rw.open = 1;
    w.rate = s->rate;
    w.channels = s->channels;
    w.bits = 24;
    w.block = s->block;
    w.frames = s->frames;
    hl = wav_header(h, &w);
    r = f_write(&rw.fo, h, hl, &n);
    if (r != FR_OK || n != hl) {
        recw_stop(r != FR_OK ? ferr(r) : "CARD FULL");
        return 0;
    }
    s->crc = 0;
    job.total = s->frames;
    job.done = 0;
    job.state = JOB_RECORDING;
    storage_error = 0;
    return 1;
}

static void recw_step(void)
{
    struct sample *s = &samples[rw.smp];
    uint32_t n = JOB_CHUNK / s->block, b;
    UINT w = 0;
    FRESULT r;

    if (rw.pos >= s->frames) {
        if (s->frames * s->block & 1u) {
            const uint8_t zero = 0;
            f_write(&rw.fo, &zero, 1, &w);
        }
        s->file_size = (uint32_t)f_size(&rw.fo);
        r = f_close(&rw.fo);
        rw.open = 0;
        recw_stop(r != FR_OK ? ferr(r) : 0);
        return;
    }
    n = n < s->frames - rw.pos ? n : s->frames - rw.pos;
    samples_regen(rw.smp, rw.pos, n, (uint8_t *)filebuf);
    b = n * s->block;
    s->crc = samples_crc32(s->crc, (const uint8_t *)filebuf, b);
    r = f_write(&rw.fo, filebuf, b, &w);
    if (r != FR_OK || w != b) {
        recw_stop(r != FR_OK ? ferr(r) : "CARD FULL");
        return;
    }
    rw.pos += n;
    job.done = rw.pos;
}

static void job_step(void)
{
    uint32_t t0 = time_us();

    if (!mounted) {
        job.state = JOB_FAILED;
        storage_error = "NO CARD";
        return;
    }
    do {
        if (job.state == JOB_IMPORTING) {
            imp_step();
        } else if (job.state == JOB_TRUNCATING) {
            trunc_step();
        } else if (job.state == JOB_RECORDING) {
            recw_step();
        } else if (job.state == JOB_COPYING) {
            dup_step();
        } else if (storage_job_active()) {
            del_step(t0);
        }
    } while (storage_job_active() && time_us() - t0 < JOB_BUDGET_US);
}

int storage_job_active(void)
{
    return job.state == JOB_COPYING || job.state == JOB_DELETING || job.state == JOB_CLEANING ||
           job.state == JOB_IMPORTING || job.state == JOB_TRUNCATING || job.state == JOB_RECORDING;
}

int storage_job_state(void)
{
    return job.state;
}

int storage_job_percent(void)
{
    return job.total ? (int)((uint64_t)job.done * 100 / job.total) : 0;
}

const char *storage_job_name(void)
{
    return job.state == JOB_CLEANING ? job.dst : job.state == JOB_DELETING ? job.src :
           job.state == JOB_IMPORTING ? (ij.cur < nimp ? imps[ij.cur].name : "") :
           job.state == JOB_TRUNCATING ? tj.out : job.dst;
}

void storage_job_cancel(void)
{
    if (job.state == JOB_COPYING) {
        job_fail("CANCELLED", JOB_CANCELLED);
    } else if (job.state == JOB_IMPORTING) {
        imp_stop("CANCELLED", JOB_CANCELLED);
    } else if (job.state == JOB_TRUNCATING) {
        trunc_stop("CANCELLED", JOB_CANCELLED);
    }
}

void storage_job_clear(void)
{
    if (!storage_job_active()) {
        job.state = JOB_NONE;
    }
}

int storage_folders(void)
{
    return nfolders;
}

const char *storage_folder_name(int i)
{
    return i >= 0 && i < nfolders ? names + name_at[i] : "";
}

const char *storage_folder(void)
{
    return folder;
}

int storage_load_folder(const char *name, int want)
{
    char dirpath[sizeof(path)];
    DIR d;
    FILINFO fi;
    int first = 0, same;

    if (!mounted) {
        storage_error = "NO CARD";
        return 0;
    }
    if (storage_job_active() || samples_take_open() || rec_state() >= REC_RECORDING) {
        storage_error = "BUSY";
        return 0;
    }
    memcpy(dirpath, "1:", 2);
    scopy(dirpath + 2, name, sizeof(dirpath) - 2);
    if (f_opendir(&d, dirpath) != FR_OK) {
        storage_error = "CAN'T OPEN FOLDER";
        return 0;
    }
    if (fprefs_due) {
        fprefs_write();
    }
    same = folder[0] && !icmp(name, folder);
    load_t0 = time_us();
    load_us = 0;
    for (int n = 0; n <= PRESET_SLOTS; n++) {
        slots[n].state = SLOT_EMPTY;
    }
    arena_used = 0;
    total = pending = changed_others = 0;
    /* Same folder again: presets are re-read, unchanged samples kept
     * (playing voices check their sample per block). */
    if (same) {
        samples_rescan(&d);
    } else {
        engine_samples_stop();
        samples_reset(name, &d);
    }
    smp_cur = -1;
    smp_demand = -1;
    smp_idle = 0;
    smp_need = smp_done = smp_load_us = 0;
    DIAG_MAIN(DT_FOLDER_SCAN, 0);
    for (uint32_t pos = d.dptr; f_readdir(&d, &fi) == FR_OK && fi.fname[0]; pos = d.dptr) {
        int n = prst_number(fi.fname);
        if (fi.fattrib & AM_DIR) {
            continue;
        }
        if (n && slots[n].state == SLOT_EMPTY) {
            slots[n].state = SLOT_PENDING;
            total++;
            pending++;
        } else if (is_wav(fi.fname)) {
            samples_add_at(fi.fname, (uint32_t)fi.fsize, pos, (uint32_t)fi.fdate << 16 | fi.ftime);
        }
    }
    f_closedir(&d);
    samples_sort();
    if (same) {
        prev_folder[0] = 0;
    } else if (folder[0]) {
        scopy(prev_folder, folder, sizeof(prev_folder));
        prev_preset = preset_number;
    }
    if (name != folder) {
        scopy(folder, name, sizeof(folder));
    }
    fprefs_read();
    storage_error = 0;
    DIAG_MAIN(DT_FOLDER_PRESETS, want);
    if (want >= 1 && want <= PRESET_SLOTS && slots[want].state == SLOT_PENDING &&
        load_slot(want) > 0) {
        first = want;
    }
    for (int n = 1; !first && n <= PRESET_SLOTS; n++) {
        if (slots[n].state == SLOT_PENDING && load_slot(n) > 0) {
            first = n;
        }
    }
    if (!first) {
        first = 1;
        slot_new(&slots[1]);
    }
    DIAG_MAIN(DT_FOLDER_LIVE, first);
    preset_number = first;
    load_live(&slots[first].p);
    engine_auto_trigger();
    bg_next = 1;
    bg_stalled = 0;
    if (!pending) {
        load_us = time_us() - load_t0;
    }
    storage_prefs_changed();
    return 1;
}

int storage_loading(void)
{
    return mounted && folder[0] && prev_folder[0] && !bg_stalled && !samples_stalled() && (pending || !smp_load_us);
}

int storage_cancel_load(void)
{
    char to[sizeof(prev_folder)];
    int ok;

    if (!storage_loading()) {
        return 0;
    }
    scopy(to, prev_folder, sizeof(to));
    ok = storage_load_folder(to, prev_preset);
    if (ok) {
        prev_folder[0] = 0;
    }
    return ok;
}

static const char *sample_name(unsigned smp)
{
    return smp && samples_valid((int)smp - 1) ? samples[smp - 1].name : 0;
}

void storage_init(void)
{
    const char *f = 0;

    yml_sample_name = sample_name;
    folder[0] = 0;
    yml_keep_init(scratch, YML_KEEP_FULL);
    slot_new(&slots[1]);
    preset_number = 1;
    set_live(&slots[1].p);
    if (!mount()) {
        return;
    }
    prefs_read();
    ssetup_read();
    storage_scan();
    for (int i = 0; i < nfolders; i++) {
        if (!icmp(storage_folder_name(i), pref_folder)) {
            f = storage_folder_name(i);
        }
    }
    if (!f && nfolders) {
        f = storage_folder_name(0);
    }
    if (f) {
        storage_load_folder(f, pref_preset);
        /* Cancelling the boot load goes to the first other folder (stock). */
        for (int i = 0; i < nfolders && !prev_folder[0]; i++) {
            if (storage_folder_name(i) != f) {
                scopy(prev_folder, storage_folder_name(i), sizeof(prev_folder));
                prev_preset = 0;
            }
        }
    }
    /* What was just read needs no rewrite. */
    prefs_due = 0;
}

/* Reads the headers of the samples presets use (only those: a folder may
 * hold thousands of WAVs), in the loading order below; 0 if the time slice
 * ran out first. */
static int read_headers(uint32_t t0)
{
    if (smp_demand >= 0 && samples[smp_demand].state == SMP_FOUND) {
        samples_probe(smp_demand);
    }
    for (int k = 0; k < PRESET_SLOTS; k++) {
        int n = (preset_number - 1 + k) % PRESET_SLOTS + 1;
        const struct preset *p = n == preset_number ? &preset : &slots[n].p;

        if (n != preset_number && slots[n].state != SLOT_READY) {
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            for (int z = 0; z < YML_ZONES && !p->ch[c].machine; z++) {
                int i = p->ch[c].s.zone[z].smp - 1;

                if (i >= 0 && samples[i].state == SMP_FOUND) {
                    samples_probe(i);
                    if (time_us() - t0 >= SAMPLE_BUDGET_US) {
                        return 0;
                    }
                }
            }
        }
    }
    return 1;
}

/* Next sample to load, as stock orders it: the live preset's, then the
 * presets after it, then those before it; machine channels need none. */
static int next_sample(void)
{
    if (smp_demand >= 0) {
        int st = samples[smp_demand].state;

        if (st == SMP_HEADER || st == SMP_LOADING) {
            return smp_demand;
        }
        smp_demand = -1;
    }
    for (int k = 0; k < PRESET_SLOTS; k++) {
        int n = (preset_number - 1 + k) % PRESET_SLOTS + 1;
        const struct preset *p = n == preset_number ? &preset : &slots[n].p;

        if (n != preset_number && slots[n].state != SLOT_READY) {
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            if (p->ch[c].machine) {
                continue;
            }
            for (int z = 0; z < YML_ZONES; z++) {
                int i = p->ch[c].s.zone[z].smp - 1;
                if (i >= 0 && (samples[i].state == SMP_HEADER || samples[i].state == SMP_LOADING)) {
                    return i;
                }
            }
        }
    }
    return -1;
}

/* Bytes the folder's needed samples take on the card, and of those done. */
static void sample_progress(void)
{
    static uint8_t seen[SAMPLES_MAX + SAMPLE_PREVIEWS];

    memset(seen, 0, sizeof(seen));
    smp_need = smp_done = 0;
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        const struct preset *p = n == preset_number ? &preset : &slots[n].p;

        if (n != preset_number && slots[n].state != SLOT_READY) {
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            for (int z = 0; z < YML_ZONES && !p->ch[c].machine; z++) {
                int i = p->ch[c].s.zone[z].smp - 1;
                const struct sample *m;

                if (i < 0 || seen[i]) {
                    continue;
                }
                m = &samples[i];
                if (m->state < SMP_HEADER || m->state > SMP_READY) {
                    continue;
                }
                seen[i] = 1;
                smp_need += m->frames * m->block;
                if (m->state == SMP_READY) {
                    smp_done += m->frames * m->block;
                }
            }
        }
    }
}

static void samples_poll(void)
{
    uint32_t t0 = time_us();

    if (smp_idle && (int32_t)(t0 - smp_recheck_at) < 0) {
        return;
    }
    if (smp_cur < 0 && !samples_stalled()) {
        DIAG_MAIN(DT_SAMPLE_HEADER, 0);
        if (!read_headers(t0)) {
            smp_idle = 0;
            return;
        }
        smp_cur = next_sample();
        sample_progress();
        if (smp_cur < 0) {
            if (!smp_idle && !smp_load_us) {
                smp_load_us = time_us() - load_t0;
            }
            smp_idle = 1;
            smp_recheck_at = t0 + SAMPLE_RECHECK_US;
            return;
        }
        smp_idle = 0;
    }
    while (smp_cur >= 0) {
        DIAG_MAIN(DT_SAMPLE_LOAD, smp_cur);
        if (samples_load_step(smp_cur)) {
            /* Memory lost to reloaded files would make room: start over. */
            if (samples[smp_cur].state == SMP_NOMEM &&
                samples_lost() + samples_pool_size() - samples_pool_used() >= samples_need(smp_cur) &&
                !storage_job_active() && rec_state() < REC_RECORDING) {
                engine_samples_stop();
                samples_repack();
            }
            smp_cur = -1;
            break;
        }
        if (time_us() - t0 >= SAMPLE_BUDGET_US) {
            break;
        }
    }
}

void storage_flush(void)
{
    if (!mounted) {
        return;
    }
    if (prefs_due) {
        prefs_due = 0;
        prefs_write();
    }
    if (ssetup_due) {
        ssetup_due = 0;
        ssetup_write();
    }
    if (fprefs_due) {
        fprefs_due = 0;
        fprefs_write();
    }
}

void storage_poll(void)
{
    uint32_t t0;

    if (storage_job_active()) {
        DIAG_MAIN(DT_STORAGE_JOB, 0);
        job_step();
    }
    t0 = time_us();

    while (pending && mounted && !bg_stalled && bg_next <= PRESET_SLOTS) {
        DIAG_MAIN(DT_PRESETS_BG, bg_next);
        if (slots[bg_next].state == SLOT_PENDING && load_slot(bg_next) < 0) {
            bg_stalled = 1;
            break;
        }
        bg_next++;
        if (!pending) {
            load_us = time_us() - load_t0;
        }
        if (time_us() - t0 >= POLL_BUDGET_US) {
            break;
        }
    }
    if (!pending && mounted && folder[0]) {
        samples_poll();
    }
    if (prefs_due && mounted && (int32_t)(time_us() - prefs_at) >= 0) {
        prefs_due = 0;
        prefs_write();
    }
    if (ssetup_due && mounted && (int32_t)(time_us() - ssetup_at) >= 0) {
        ssetup_due = 0;
        ssetup_write();
    }
    if (fprefs_due && mounted && (int32_t)(time_us() - fprefs_at) >= 0) {
        fprefs_due = 0;
        fprefs_write();
    }
}

int storage_total(void)
{
    return total;
}

int storage_pending(void)
{
    return pending;
}

uint32_t storage_load_us(void)
{
    return load_us;
}

int storage_sample(int slot, int ch, int zone)
{
    if (slot < 1 || slot > PRESET_SLOTS) {
        return -1;
    }
    return (slot == preset_number ? &preset : &slots[slot].p)->ch[ch].s.zone[zone].smp - 1;
}

int storage_samples_percent(void)
{
    uint32_t done = smp_done;

    if (!folder[0] || pending || (smp_idle && smp_cur < 0) || samples_stalled()) {
        return -1;
    }
    if (smp_cur >= 0) {
        done += samples[smp_cur].loaded * samples[smp_cur].block;
    }
    done = smp_need ? (uint32_t)((uint64_t)done * 100u / smp_need) : 0;
    return done > 99 ? 99 : (int)done;
}

int storage_sample_loading(void)
{
    return smp_cur;
}

uint32_t storage_sample_load_us(void)
{
    return smp_load_us;
}

int storage_usage(uint32_t *total_kb, uint32_t *free_kb)
{
    DWORD nclst;
    FATFS *f;

    *total_kb = *free_kb = 0;
    if (!mounted || f_getfree("1:", &nclst, &f) != FR_OK) {
        return 0;
    }
    *total_kb = (uint32_t)(f->n_fatent - 2) * f->csize / 2;
    *free_kb = nclst * f->csize / 2;
    return 1;
}

/* ---- Presets ---- */

int storage_state(int n)
{
    if (n < 1 || n > PRESET_SLOTS) {
        return SLOT_EMPTY;
    }
    return n == preset_number ? SLOT_READY : slots[n].state;
}

const char *storage_name(int n)
{
    if (n == preset_number) {
        return preset.name;
    }
    return storage_state(n) == SLOT_READY ? slots[n].p.name : 0;
}

int storage_changed(int n)
{
    if (n == preset_number) {
        return live_changed();
    }
    return storage_state(n) == SLOT_READY && slots[n].changed;
}

int storage_blank(void)
{
    return !slots[preset_number].on_card && !live_changed();
}

int storage_changed_count(void)
{
    return changed_others + live_changed();
}

int storage_next(int from, int dir, int all)
{
    for (int c = from + dir; c >= 1 && c <= PRESET_SLOTS; c += dir) {
        if (all || storage_state(c) != SLOT_EMPTY) {
            return c;
        }
    }
    return from;
}

const struct preset *storage_peek(int n)
{
    if (n == preset_number) {
        return &preset;
    }
    if (n < 1 || n > PRESET_SLOTS) {
        return 0;
    }
    if (slots[n].state == SLOT_PENDING && mounted && load_slot(n) < 0) {
        return 0;
    }
    return slots[n].state == SLOT_READY ? &slots[n].p : 0;
}

int storage_put(int n, const struct preset *p)
{
    struct slot *s;
    int was, now;

    if (n < 1 || n > PRESET_SLOTS) {
        return 0;
    }
    if (n == preset_number) {
        set_live(p);
        return 1;
    }
    s = &slots[n];
    if (s->state == SLOT_PENDING && load_slot(n) < 0) {
        return 0;
    }
    if (s->state == SLOT_EMPTY) {
        slot_new(s);
    }
    was = s->changed;
    s->p = *p;
    now = differs(s, &s->p);
    changed_others += now - was;
    s->changed = (uint8_t)now;
    return 1;
}

int storage_new_slot(void)
{
    int last = 0;

    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (storage_state(n) != SLOT_EMPTY) {
            last = n;
        }
    }
    if (last < PRESET_SLOTS) {
        return last + 1;
    }
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (storage_state(n) == SLOT_EMPTY) {
            return n;
        }
    }
    return 0;
}

/* Puts the live preset back into its slot. */
static void store_live(void)
{
    struct slot *o = &slots[preset_number];

    o->p = preset;
    o->changed = differs(o, &o->p);
    if (o->changed) {
        changed_others++;
    } else if (!o->on_card) {
        o->state = SLOT_EMPTY;      /* an untouched new preset vanishes */
    }
}

static void take_live(int n)
{
    struct slot *s = &slots[n];

    if (s->changed) {
        changed_others--;
        s->changed = 0;
    }
    DIAG_MAIN(DT_PRESET_SELECT, n);
    preset_number = n;
    load_live(&s->p);
    engine_auto_trigger();
    smp_idle = 0;
}

int storage_select(int n)
{
    struct slot *s;

    if (n < 1 || n > PRESET_SLOTS) {
        return 0;
    }
    if (n == preset_number) {
        return 1;
    }
    s = &slots[n];
    if (s->state == SLOT_PENDING) {
        if (!mounted) {
            storage_error = "NO CARD";
            return 0;
        }
        if (load_slot(n) < 0) {
            return 0;
        }
    }
    store_live();
    if (s->state == SLOT_EMPTY) {
        slot_new(s);
    }
    take_live(n);
    storage_prefs_changed();
    return 1;
}

/* Inc/Dec by CV stop at an empty slot; Reset goes to the first preset of
 * the run of presets holding the live one. */
static int cv_target(int action)
{
    int n = preset_number;

    if (action == PCV_RESET) {
        while (n > 1 && storage_state(n - 1) != SLOT_EMPTY) {
            n--;
        }
        return n;
    }
    n += action == PCV_INC ? 1 : -1;
    return storage_state(n) != SLOT_EMPTY ? n : preset_number;
}

int storage_cv_poll(void)
{
    static uint32_t seen[PCV_COUNT];
    int moved = 0;

    for (int i = 0; i < PCV_COUNT; i++) {
        uint32_t c = engine_status.preset_cv[i];
        while (seen[i] != c) {
            seen[i]++;
            int n = cv_target(i);
            if (n != preset_number && storage_select(n)) {
                moved = 1;
            }
        }
    }
    return moved;
}

int storage_save(int n)
{
    const struct yml_keep *k;
    int src = preset_number;
    struct slot *d = &slots[n];

    if (n < 1 || n > PRESET_SLOTS || !can_write() || !keep_for(src, &k)) {
        return 0;
    }
    if (!write_slot(n, &preset, k)) {
        return 0;
    }
    storage_error = 0;
    if (n != src) {
        struct slot *s = &slots[src];

        if (s->on_card) {
            s->p = s->saved;
            s->changed = s->force;
            changed_others += s->force;
        } else {
            s->state = SLOT_EMPTY;
        }
        if (d->state == SLOT_PENDING) {
            pending--;
        }
        if (d->state == SLOT_READY && d->changed) {
            changed_others--;
        }
        d->keep = s->keep;
        d->keep_gone = s->keep_gone;
        preset_number = n;
        storage_prefs_changed();
    }
    d->state = SLOT_READY;
    d->on_card = 1;
    d->changed = 0;
    d->force = 0;
    d->saved = preset;
    d->p = preset;
    return 1;
}

int storage_save_all(void)
{
    int done = 0;

    if (!can_write()) {
        return -1;
    }
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        struct slot *s = &slots[n];
        const struct yml_keep *k;

        if (!storage_changed(n)) {
            continue;
        }
        const struct preset *p = n == preset_number ? &preset : &s->p;
        if (!keep_for(n, &k) || !write_slot(n, p, k)) {
            return -1;
        }
        s->saved = *p;
        s->p = *p;
        s->on_card = 1;
        s->force = 0;
        if (n != preset_number) {
            s->changed = 0;
            changed_others--;
        }
        done++;
    }
    storage_error = 0;
    return done;
}

void storage_revert(void)
{
    load_live(&slots[preset_number].saved);
}

void storage_revert_channel(int ch)
{
    static struct preset p;

    p = preset;
    p.ch[ch] = slots[preset_number].saved.ch[ch];
    set_live(&p);
}

int storage_erase(void)
{
    struct slot *s = &slots[preset_number];

    if (!can_write()) {
        return 0;
    }
    if (s->on_card) {
        FRESULT r = f_unlink(folder_path(preset_number, 0));
        if (r != FR_OK && r != FR_NO_FILE) {
            storage_error = "CARD ERASE FAILED";
            return 0;
        }
    }
    slot_new(s);
    set_live(&s->p);
    storage_error = 0;
    return 1;
}

/* ---- Another folder's presets ---- */

static char xfolder[FF_LFN_BUF + 1];
static uint16_t xslots[PRESET_SLOTS];
static int nx;
static char xnames[PRESET_SLOTS + 1][PRESET_NAME_LEN + 1];
static uint8_t xnamed[PRESET_SLOTS + 1];
static struct preset xp;
static struct yml_samples xsamp;
static int xp_slot;

static int xread(int n)
{
    char *o = path;
    UINT len;

    xp_slot = 0;
    memcpy(o, "1:", 2);
    scopy(o + 2, xfolder, sizeof(path) - 20);
    o += slen(o);
    *o++ = '/';
    memcpy(o, "prst", 4);
    memcpy(put3(o + 4, n), ".yml", 5);
    if (read_file(path, &len) != FR_OK || yml_parse(filebuf, len, &xp, scratch) != YML_OK) {
        return 0;
    }
    yml_samples(scratch, &xsamp);
    return 1;
}

int storage_xopen(const char *name)
{
    static uint8_t seen[PRESET_SLOTS + 1];
    DIR d;
    FILINFO fi;

    nx = 0;
    xp_slot = 0;
    memset(xnamed, 0, sizeof(xnamed));
    memset(seen, 0, sizeof(seen));
    scopy(xfolder, name, sizeof(xfolder));
    scopy(path, "1:", sizeof(path));
    scopy(path + 2, name, sizeof(path) - 2);
    if (!mounted || f_opendir(&d, path) != FR_OK) {
        return 0;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        int n = prst_number(fi.fname);
        if (n && !(fi.fattrib & AM_DIR)) {
            seen[n] = 1;
        }
    }
    f_closedir(&d);
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (seen[n]) {
            xslots[nx++] = (uint16_t)n;
        }
    }
    return nx;
}

int storage_xcount(void)
{
    return nx;
}

int storage_xslot(int i)
{
    return i >= 0 && i < nx ? xslots[i] : 0;
}

const char *storage_xname(int i)
{
    int n = storage_xslot(i);

    if (!n) {
        return "";
    }
    if (!xnamed[n]) {
        xnamed[n] = 1;
        xnames[n][0] = 0;
        if (xread(n)) {
            scopy(xnames[n], xp.name, sizeof(xnames[n]));
            xp_slot = n;
        }
    }
    return xnames[n];
}

const struct preset *storage_xpreset(int i)
{
    int n = storage_xslot(i);

    if (!n) {
        return 0;
    }
    if (xp_slot != n) {
        if (!xread(n)) {
            return 0;
        }
        xp_slot = n;
    }
    return &xp;
}

/* ---- Samples: demand loading, use, rename, erase ---- */

void storage_sample_demand(int i)
{
    if (samples_valid(i)) {
        smp_demand = i;
        smp_idle = 0;
    }
}

int storage_sample_used(int i)
{
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        const struct preset *p = n == preset_number ? &preset : &slots[n].p;

        if (n != preset_number && slots[n].state != SLOT_READY) {
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            for (int z = 0; z < ZONES; z++) {
                if (p->ch[c].s.zone[z].smp == i + 1) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

void storage_sample_unload(int i)
{
    if (samples_unload(i) && smp_cur == i) {
        smp_cur = -1;
    }
}

void storage_preview_release(int i)
{
    if (smp_cur == i) {
        smp_cur = -1;
    }
    if (smp_demand == i) {
        smp_demand = -1;
    }
    samples_preview_release(i);
}

void storage_preview_release_all(void)
{
    int i;

    while ((i = samples_preview_next()) >= 0) {
        storage_preview_release(i);
    }
}

static void loaded_all(void)
{
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (slots[n].state == SLOT_PENDING && load_slot(n) < 0) {
            break;
        }
    }
}

/* A presets file's reference to a sample changed: it needs saving. */
static void mark_slot(int n)
{
    struct slot *s = &slots[n];

    s->force = 1;
    if (n != preset_number && !s->changed) {
        s->changed = 1;
        changed_others++;
    }
}

static int file_name_ok(const char *name, const char *self)
{
    FILINFO fi;
    size_t n = slen(name);
    FRESULT r;

    if (n < 5 || n > SAMPLE_NAME_LEN || !is_wav(name) || name[0] == '.' || name[0] == ' ') {
        storage_error = "BAD NAME";
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)name[i] < 0x20 || strchr("\"*/:<>?\\|", name[i])) {
            storage_error = "BAD NAME";
            return 0;
        }
    }
    if (is_wav(name) && prst_number(name)) {
        storage_error = "BAD NAME";
        return 0;
    }
    scopy(jp1, folder_path(0, name), sizeof(jp1));
    r = f_stat(jp1, &fi);
    if (r == FR_OK && icmp(self, name)) {
        storage_error = "FILE EXISTS";
        return 0;
    }
    if (r != FR_OK && r != FR_NO_FILE) {
        storage_error = ferr(r);
        return 0;
    }
    return 1;
}

int storage_sample_rename(int i, const char *to)
{
    char from[SAMPLE_NAME_LEN + 1];
    FRESULT r;

    if (!folder_ready() || !samples_valid(i) || i >= SAMPLES_MAX) {
        return 0;
    }
    scopy(from, samples[i].name, sizeof(from));
    if (!file_name_ok(to, from)) {
        return 0;
    }
    if (!memcmp(from, to, slen(from) + 1)) {
        return 1;
    }
    scopy(jp1, folder_path(0, from), sizeof(jp1));
    scopy(jp2, folder_path(0, to), sizeof(jp2));
    if (!icmp(from, to)) {
        char tmp[sizeof(path)];

        scopy(tmp, folder_path(0, "a8m-ren.tmp"), sizeof(tmp));
        r = f_rename(jp1, tmp);
        if (r == FR_OK) {
            r = f_rename(tmp, jp2);
            if (r != FR_OK) {
                f_rename(tmp, jp1);
            }
        }
    } else {
        r = f_rename(jp1, jp2);
    }
    if (r != FR_OK) {
        storage_error = ferr(r);
        return 0;
    }
    samples_rename(i, to);
    loaded_all();
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        if (slots[n].state == SLOT_READY) {
            const struct preset *p = n == preset_number ? &preset : &slots[n].p;
            for (int c = 0; c < CHANNELS; c++) {
                for (int z = 0; z < ZONES; z++) {
                    if (p->ch[c].s.zone[z].smp == i + 1) {
                        mark_slot(n);
                    }
                }
            }
        }
    }
    storage_error = 0;
    return 1;
}

int storage_sample_erase(int i)
{
    FRESULT r;

    if (!folder_ready() || !samples_valid(i) || i >= SAMPLES_MAX) {
        return 0;
    }
    scopy(jp1, folder_path(0, samples[i].name), sizeof(jp1));
    f_chmod(jp1, 0, AM_RDO);
    r = f_unlink(jp1);
    if (r != FR_OK && r != FR_NO_FILE) {
        storage_error = ferr(r);
        return 0;
    }
    samples_forget(i);
    if (smp_cur == i) {
        smp_cur = -1;
    }
    loaded_all();
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        struct slot *s = &slots[n];

        if (s->state != SLOT_READY) {
            continue;
        }
        if (n == preset_number) {
            static struct preset q;

            q = preset;
            if (preset_drop_sample(&q, (unsigned)i + 1)) {
                preset_drop_sample(&s->saved, (unsigned)i + 1);
                set_live(&q);
                mark_slot(n);
            }
        } else if (preset_drop_sample(&s->p, (unsigned)i + 1)) {
            preset_drop_sample(&s->saved, (unsigned)i + 1);
            mark_slot(n);
        }
    }
    storage_error = 0;
    return 1;
}

int storage_sample_uses(int i)
{
    int n = 0;

    loaded_all();
    for (int s = 1; s <= PRESET_SLOTS; s++) {
        const struct preset *p = s == preset_number ? &preset : &slots[s].p;

        if (s != preset_number && slots[s].state != SLOT_READY) {
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) {
            for (int z = 0; z < ZONES; z++) {
                n += p->ch[c].s.zone[z].smp == i + 1;
            }
        }
    }
    return n;
}

static int trim_preset(struct preset *p, unsigned smp, uint32_t from, uint32_t to, uint32_t old_frames)
{
    int n = 0;

    for (int c = 0; c < CHANNELS; c++) {
        for (int z = 0; z < ZONES; z++) {
            if (p->ch[c].s.zone[z].smp == smp) {
                zone_trim(&p->ch[c].s.zone[z], from, to, old_frames);
                n++;
            }
        }
    }
    return n;
}

void storage_sample_trimmed(int i, uint32_t from, uint32_t to, uint32_t old_frames)
{
    loaded_all();
    for (int n = 1; n <= PRESET_SLOTS; n++) {
        struct slot *s = &slots[n];

        if (s->state != SLOT_READY) {
            continue;
        }
        if (n == preset_number) {
            static struct preset q;

            q = preset;
            if (trim_preset(&q, (unsigned)i + 1, from, to, old_frames)) {
                trim_preset(&s->saved, (unsigned)i + 1, from, to, old_frames);
                set_live(&q);
                mark_slot(n);
            }
        } else if (trim_preset(&s->p, (unsigned)i + 1, from, to, old_frames)) {
            trim_preset(&s->saved, (unsigned)i + 1, from, to, old_frames);
            mark_slot(n);
        }
    }
}

/* ---- Card browser: any folder, subfolders first, then .wav files ---- */

struct bent {
    char name[SAMPLE_NAME_LEN + 1];
    uint8_t dir;
    uint32_t size, pos;
};

static struct bent bents[BENT_MAX], bents_tmp[BENT_MAX];
static int nbents;
static DIR bdir;
static char bpath[FF_LFN_BUF + 4];

static int bent_less(const struct bent *a, const struct bent *b)
{
    return a->dir != b->dir ? a->dir > b->dir : samples_name_cmp(a->name, b->name) < 0;
}

static void bents_sort(void)
{
    struct bent *a = bents, *b = bents_tmp, *t;

    for (int w = 1; w < nbents; w *= 2) {
        for (int lo = 0; lo < nbents; lo += 2 * w) {
            int mid = lo + w < nbents ? lo + w : nbents, hi = lo + 2 * w < nbents ? lo + 2 * w : nbents;
            int i = lo, j = mid, k = lo;

            while (i < mid && j < hi) {
                b[k++] = bent_less(&a[j], &a[i]) ? a[j++] : a[i++];
            }
            while (i < mid) {
                b[k++] = a[i++];
            }
            while (j < hi) {
                b[k++] = a[j++];
            }
        }
        t = a;
        a = b;
        b = t;
    }
    if (a != bents) {
        memcpy(bents, a, sizeof(bents[0]) * (unsigned)nbents);
    }
}

int storage_bopen(const char *dir)
{
    DIR d;
    FILINFO fi;

    nbents = 0;
    if (!mounted) {
        return 0;
    }
    imp_path(jp1, dir, "");
    if (dir[0]) {
        jp1[slen(jp1) - 1] = 0;
    }
    if (f_opendir(&d, jp1) != FR_OK) {
        return 0;
    }
    bdir = d;
    scopy(bpath, dir, sizeof(bpath));
    for (uint32_t pos = d.dptr; f_readdir(&d, &fi) == FR_OK && fi.fname[0] && nbents < BENT_MAX; pos = d.dptr) {
        struct bent b;

        if ((fi.fattrib & (AM_HID | AM_SYS)) || slen(fi.fname) > SAMPLE_NAME_LEN ||
            ((fi.fattrib & AM_DIR) ? !icmp(fi.fname, "A8M") : !is_wav(fi.fname))) {
            continue;
        }
        scopy(b.name, fi.fname, sizeof(b.name));
        b.dir = (fi.fattrib & AM_DIR) != 0;
        b.size = (uint32_t)fi.fsize;
        b.pos = pos;
        bents[nbents++] = b;
    }
    f_closedir(&d);
    bents_sort();
    return 1;
}

int storage_bcount(void)
{
    return nbents;
}

const char *storage_bname(int i)
{
    return i >= 0 && i < nbents ? bents[i].name : "";
}

int storage_bisdir(int i)
{
    return i >= 0 && i < nbents && bents[i].dir;
}

uint32_t storage_bsize(int i)
{
    return i >= 0 && i < nbents ? bents[i].size : 0;
}

int storage_bpreview(int i)
{
    if (i < 0 || i >= nbents || bents[i].dir) {
        return -1;
    }
    return samples_preview_at(bpath, bents[i].name, &bdir, bents[i].pos);
}

const char *storage_xsample(int ch, int z)
{
    return xsamp.name[ch][z];
}
