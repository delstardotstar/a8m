#include "update.h"

#include <string.h>
#include "ff.h"
#include "hw.h"
#include "mmc.h"
#include "diag.h"
#include "audio.h"
#include "storage.h"
#include "samples.h"
#include "rec.h"
#include "systime.h"
#include "gfx.h"
#include "oled.h"

#define SRC         "1:app"
#define DST_NEW     "0:app.new"
#define DST         "0:app"
#define DST_BAK     "0:app.bak"

#define LOAD_ADDRESS    0x80000000u
#define MIN_BYTES       0x100u
#define MAX_BYTES       (16u << 20)
#define VERSION_AT      (8u + 0x40u)    /* fw_version (main.c, link.ld) */
#define CHUNK           32768u
#define BUDGET_US       20000u
#define RESET_WAIT_US   1000000u
#define PRM_RSTCTRL     0x44e00f00u
#define RST_GLOBAL_COLD_SW 2u     /* what stock uses for its reboot (seen in the emulator) */

/* Stock builds by length and CRC-32 of the whole file. */
static const struct { uint32_t len, crc; const char *name; } stock_builds[] = {
    { 637648u, 0x1f76d6bdu, "STOCK 3.04" },
    { 637520u, 0xc2be1e3cu, "STOCK 3.02" },
    { 637296u, 0xc0422d65u, "STOCK 3.00" },
};

const char *update_error;
static int state = UPD_IDLE, kind;
static uint32_t len, done, want_crc;
static char found[32];
static FIL fi, fo;
static int fi_open, fo_open;
static FATFS som;
static uint8_t buf[CHUNK] __attribute__((aligned(64)));

/* Scan: CRC of the whole file, CRC of the payload but its last word, the
 * header, the version text, the check block's magic and the last word. */
static uint32_t crc_all, crc_body, hdr[2], magic, last;
static char version[32];

static uint32_t crc_table[256];

static uint32_t crc_update(uint32_t c, const uint8_t *p, uint32_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) {
                v = (v >> 1) ^ (0xedb88320u & -(v & 1u));
            }
            crc_table[i] = v;
        }
    }
    c = ~c;
    while (n--) {
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    }
    return ~c;
}

/* The bytes of [at, at + n) that lie in the chunk read at offset off. */
static void capture(void *dst, uint32_t at, uint32_t n, uint32_t off, const uint8_t *p, uint32_t got)
{
    for (uint32_t i = 0; i < n; i++) {
        if (at + i >= off && at + i < off + got) {
            ((uint8_t *)dst)[i] = p[at + i - off];
        }
    }
}

/* Load Software's boot card writes, only for the three names above
 * (cal.c has the other ones). */
static void som_writes(int on)
{
    mmc_som_writable = on;
}

static int som_mount(void)
{
    f_mount(0, "0:", 0);
    return f_mount(&som, "0:", 1) == FR_OK;
}

static void close_files(void)
{
    if (fi_open) {
        f_close(&fi);
        fi_open = 0;
    }
    if (fo_open) {
        som_writes(1);
        f_close(&fo);
        som_writes(0);
        fo_open = 0;
    }
}

static void fail(const char *why)
{
    close_files();
    if (state == UPD_COPYING || state == UPD_VERIFYING) {
        som_writes(1);
        f_unlink(DST_NEW);
        som_writes(0);
    }
    update_error = why;
    state = state == UPD_SCANNING ? UPD_BAD : UPD_FAILED;
}

static const char *ferr(FRESULT r, const char *fallback)
{
    switch (r) {
    case FR_WRITE_PROTECTED: return "BOOT CARD LOCKED";
    case FR_NOT_READY: case FR_DISK_ERR: return "CARD ERROR";
    case FR_DENIED: return "BOOT CARD FULL";
    default: return fallback;
    }
}

void update_clear(void)
{
    if (!update_busy() && state != UPD_DONE) {
        close_files();
        state = UPD_IDLE;
        update_error = 0;
    }
}

void update_find(void)
{
    FRESULT r;

    close_files();
    update_error = 0;
    found[0] = 0;
    if (!storage_card()) {
        update_error = "NO CARD";
        state = UPD_NONE;
        return;
    }
    r = f_open(&fi, SRC, FA_READ);
    if (r == FR_NO_FILE || r == FR_NO_PATH) {
        update_error = "NO APP ON CARD";
        state = UPD_NONE;
        return;
    }
    state = UPD_SCANNING;
    if (r != FR_OK) {
        fail(ferr(r, "CAN'T READ APP"));
        return;
    }
    fi_open = 1;
    len = (uint32_t)f_size(&fi);
    if (len < MIN_BYTES || len > MAX_BYTES) {
        fail(len < MIN_BYTES ? "APP TOO SHORT" : "APP TOO BIG");
        return;
    }
    done = 0;
    crc_all = crc_body = 0;
    hdr[0] = hdr[1] = magic = last = 0;
    memset(version, 0, sizeof(version));
}

static void identify(void)
{
    uint32_t payload = len - 8;

    if (hdr[1] != LOAD_ADDRESS || (hdr[0] != len && hdr[0] != payload)) {
        fail("BAD HEADER");
        return;
    }
    if (payload > IMAGE_CHECK_BYTES && magic == IMAGE_CHECK_MAGIC) {
        if (crc_body != last) {
            fail("DAMAGED A8M APP");
            return;
        }
        kind = APP_OURS;
        version[sizeof(version) - 1] = 0;
        if (!memcmp(version, "A8M ", 4) && version[4]) {
            for (int i = 0; version[i]; i++) {
                if (version[i] < ' ' || version[i] > '~') {
                    version[i] = 0;
                    break;
                }
            }
            memcpy(found, version, sizeof(found));
        } else {
            memcpy(found, "A8M (OLDER BUILD)", 18);
        }
    } else {
        kind = APP_UNKNOWN;
        memcpy(found, "UNKNOWN APP", 12);
        for (unsigned i = 0; i < sizeof(stock_builds) / sizeof(stock_builds[0]); i++) {
            if (stock_builds[i].len == len && stock_builds[i].crc == crc_all) {
                kind = APP_STOCK;
                memcpy(found, stock_builds[i].name, strlen(stock_builds[i].name) + 1);
            }
        }
    }
    close_files();
    want_crc = crc_all;
    state = UPD_FOUND;
}

static void scan_step(void)
{
    UINT got = 0;
    FRESULT r = f_read(&fi, buf, CHUNK, &got);
    uint32_t off = done, check_at = len - IMAGE_CHECK_BYTES;

    if (r != FR_OK || (got < CHUNK && done + got != len)) {
        fail(ferr(r, "CAN'T READ APP"));
        return;
    }
    crc_all = crc_update(crc_all, buf, got);
    {
        uint32_t a = off < 8 ? 8 : off, b = off + got < len - 4 ? off + got : len - 4;
        if (b > a) {
            crc_body = crc_update(crc_body, buf + (a - off), b - a);
        }
    }
    capture(hdr, 0, 8, off, buf, got);
    capture(version, VERSION_AT, sizeof(version), off, buf, got);
    if (len > IMAGE_CHECK_BYTES + 8) {
        capture(&magic, check_at, 4, off, buf, got);
    }
    capture(&last, len - 4, 4, off, buf, got);
    done += got;
    if (done >= len) {
        identify();
    }
}

int update_install(void)
{
    FRESULT r;

    update_error = 0;
    if (state != UPD_FOUND) {
        return 0;
    }
    if (storage_job_active() || samples_take_open() || rec_state() >= REC_RECORDING) {
        update_error = "BUSY";
        return 0;
    }
    if (!som_mount()) {
        update_error = "NO BOOT CARD";
        return 0;
    }
    if ((r = f_open(&fi, SRC, FA_READ)) != FR_OK) {
        update_error = ferr(r, "CAN'T READ APP");
        return 0;
    }
    fi_open = 1;
    som_writes(1);
    r = f_open(&fo, DST_NEW, FA_WRITE | FA_CREATE_ALWAYS);
    som_writes(0);
    state = UPD_COPYING;
    if (r != FR_OK) {
        fail(ferr(r, "CAN'T WRITE BOOT CARD"));
        return 0;
    }
    fo_open = 1;
    done = 0;
    crc_all = 0;
    return 1;
}

static void verify_begin(void)
{
    FRESULT r;

    som_writes(1);
    r = f_close(&fo);
    som_writes(0);
    fo_open = 0;
    f_close(&fi);
    fi_open = 0;
    if (r != FR_OK) {
        fail(ferr(r, "WRITE FAILED"));
        return;
    }
    state = UPD_VERIFYING;
    /* Remounted so the read-back comes from the card, not FatFs's buffers. */
    if (!som_mount() || (r = f_open(&fi, DST_NEW, FA_READ)) != FR_OK) {
        fail("VERIFY FAILED");
        return;
    }
    fi_open = 1;
    if ((uint32_t)f_size(&fi) != len) {
        fail("VERIFY FAILED");
        return;
    }
    done = 0;
    crc_all = 0;
}

static void copy_step(void)
{
    UINT got = 0, put = 0;
    FRESULT r = f_read(&fi, buf, CHUNK, &got);

    if (r != FR_OK || (got < CHUNK && done + got != len)) {
        fail(ferr(r, "CAN'T READ APP"));
        return;
    }
    crc_all = crc_update(crc_all, buf, got);
    som_writes(1);
    r = f_write(&fo, buf, got, &put);
    som_writes(0);
    if (r != FR_OK || put != got) {
        fail(r == FR_OK ? "BOOT CARD FULL" : ferr(r, "WRITE FAILED"));
        return;
    }
    done += got;
    if (done >= len) {
        if (crc_all != want_crc) {
            fail("APP CHANGED ON CARD");
            return;
        }
        verify_begin();
    }
}

static void swap(void)
{
    FRESULT r;
    FILINFO st;
    int had_app;

    close_files();
    som_writes(1);
    r = f_unlink(DST_BAK);
    if (r != FR_OK && r != FR_NO_FILE) {
        fail(ferr(r, "CAN'T REMOVE APP.BAK"));
        return;
    }
    r = f_rename(DST, DST_BAK);
    had_app = r == FR_OK;
    if (r != FR_OK && r != FR_NO_FILE) {
        fail(ferr(r, "CAN'T RENAME APP"));
        return;
    }
    r = f_rename(DST_NEW, DST);
    if (r != FR_OK) {
        if (had_app && f_rename(DST_BAK, DST) != FR_OK) {
            som_writes(0);
            update_error = "CHECK THE BOOT CARD";
            state = UPD_FAILED;
            return;
        }
        fail(ferr(r, "CAN'T RENAME APP.NEW"));
        return;
    }
    som_writes(0);
    if (!som_mount() || f_stat(DST, &st) != FR_OK || (uint32_t)st.fsize != len) {
        update_error = "CHECK THE BOOT CARD";
        state = UPD_FAILED;
        return;
    }
    state = UPD_DONE;
}

static void verify_step(void)
{
    UINT got = 0;
    FRESULT r = f_read(&fi, buf, CHUNK, &got);

    if (r != FR_OK || (got < CHUNK && done + got != len)) {
        fail("VERIFY FAILED");
        return;
    }
    crc_all = crc_update(crc_all, buf, got);
    done += got;
    if (done >= len) {
        if (crc_all != want_crc) {
            fail("VERIFY FAILED");
            return;
        }
        swap();
    }
}

void update_poll(void)
{
    uint32_t t0 = time_us();

    while (update_busy() && time_us() - t0 < BUDGET_US) {
        switch (state) {
        case UPD_SCANNING: scan_step(); break;
        case UPD_COPYING: copy_step(); break;
        case UPD_VERIFYING: verify_step(); break;
        }
    }
}

void update_cancel(void)
{
    if (update_busy()) {
        fail("CANCELLED");
        state = UPD_IDLE;
    }
}

int update_state(void) { return state; }
int update_kind(void) { return kind; }
uint32_t update_size(void) { return len; }
const char *update_found(void) { return found; }

int update_busy(void)
{
    return state == UPD_SCANNING || state == UPD_COPYING || state == UPD_VERIFYING;
}

int update_percent(void)
{
    uint32_t p = len ? (uint32_t)((uint64_t)done * 100u / len) : 0;

    return state == UPD_COPYING ? (int)(p * 3u / 4u) : state == UPD_VERIFYING ? (int)(75u + p / 4u) :
           state == UPD_DONE ? 100 : (int)p;
}

static void spin_us(uint32_t us)
{
    uint32_t t0 = time_ticks();

    while (time_ticks() - t0 < us * 24u) {
    }
}

void update_reset(void)
{
    storage_flush();
    f_mount(0, "0:", 0);
    audio_mute();
    spin_us(50000);
    /* Caches and MMU stay on: the reset doesn't need them off, and turning
     * the D-cache off with dirty lines in it corrupted the stack on
     * hardware. The card writes are done (mmc.c cleans its DMA buffers). */
    __asm__ volatile("cpsid if" ::: "memory");
    REG(PRM_RSTCTRL) = RST_GLOBAL_COLD_SW;
    __asm__ volatile("dsb\n isb" ::: "memory");
    spin_us(RESET_WAIT_US);
    gfx_clear();
    gfx_title(0, 2, "LOAD SOFTWARE", C_TITLE);
    gfx_text(4, 30, "INSTALLED", C_GREEN);
    gfx_text(4, 50, "RESET DIDN'T HAPPEN:", C_WHITE);
    gfx_title(4, 66, "POWER CYCLE", C_YELLOW);
    gfx_text(4, 90, "TURN THE MODULE OFF", C_DIM);
    gfx_text(4, 100, "AND ON AGAIN", C_DIM);
    oled_flush();
    for (;;) {
    }
}
