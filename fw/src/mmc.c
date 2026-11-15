/*
 * SD cards, polled (no DMA, no interrupts): only the main loop uses them,
 * for small files. Unit 0 = the SOM boot card (MMC0, read only but for
 * Load Software's and Calibrate's writes, mmc_som_writable),
 * unit 1 = the front card (MMC2). Bring-up values are stock's (traced in
 * the emulator with A8_MMC_TRACE=1): 400 kHz identification, CMD0, CMD8,
 * ACMD41 until ready, CMD2, CMD3, CMD16, CMD7, ACMD6 (4-bit), then 24 MHz.
 * Short reads and all writes move one block per command through the data
 * register; reads of BULK_MIN blocks or more are one CMD18 (auto CMD12),
 * by the CPU or (mmc_use_dma) by EDMA on the front card when the buffer is
 * line-aligned.
 */
#include "mmc.h"

#include <string.h>
#include "hw.h"
#include "mmu.h"
#include "systime.h"
#include "wdt.h"

static const uint32_t base_of[MMC_UNITS] = { 0x48060000u, 0x47810000u };
static const uint32_t clk_of[MMC_UNITS] = { 0x03cu, 0x0f8u };       /* CM_PER_MMC0/2 */
static uint32_t base;       /* the unit being driven (set by select()) */
int mmc_som_writable;
#define SYSCONFIG       0x110u
#define SYSSTATUS       0x114u
#define CON             0x12cu
#define BLK             0x204u
#define ARG             0x208u
#define CMD             0x20cu
#define RSP10           0x210u
#define DATA            0x220u
#define PSTATE          0x224u
#define HCTL            0x228u
#define SYSCTL          0x22cu
#define STAT            0x230u
#define IE              0x234u
#define CAPA            0x240u

#define CON_INIT        0x2u
#define HCTL_3V0        0x0c00u         /* SDVS 3.0 V */
#define HCTL_SDBP       0x0100u         /* bus power */
#define HCTL_DTW4       0x0002u
#define SYSCTL_400K     0x00003c07u     /* CLKD 240, CEN, ICS, ICE */
#define SYSCTL_24M      0x000e0107u     /* CLKD 4, DTO max */
#define SYSCTL_SRC      (1u << 25)
#define SYSCTL_SRD      (1u << 26)
#define STAT_CC         0x1u
#define STAT_TC         0x2u
#define STAT_BWR        0x10u
#define STAT_BRR        0x20u
#define STAT_ERRI       0x8000u
#define PSTATE_CMDI     0x1u
#define PSTATE_DATI     0x2u

#define RSP_NONE        0u
#define RSP_136         (1u << 16)
#define RSP_48          (2u << 16)
#define RSP_48B         (3u << 16)
#define DP              (1u << 21)
#define DDIR_READ       (1u << 4)
#define CMD_DE          (1u << 0)
#define CMD_BCE         (1u << 1)
#define CMD_ACMD12      (1u << 2)
#define CMD_MSBS        (1u << 5)
#define CMD_INDEX(n)    ((uint32_t)(n) << 24)

#define TIMEOUT_US      250000u

#ifdef MMC_SINGLE
#define BULK_MIN        0xffffffffu
#else
#define BULK_MIN        8u
#endif
#define BULK_MAX        2048u       /* blocks per command */

/* EDMA for front card reads: MMC2's requests reach events 2 (TX) and 3 (RX)
 * through the crossbar, as stock maps them. Queue 1, so bulk reads never
 * hold up the audio and CV transfers on queue 0. Completion is polled
 * (IPR bit, not enabled in IER). */
#define TPCC            0x49000000u
#define DCHMAP(ch)      (0x100u + 4u * (ch))
#define DMAQNUM0        0x240u
#define DRAE0           0x340u
#define EMCR            0x308u
#define SH0_ECR         0x2008u
#define SH0_EECR        0x2028u
#define SH0_EESR        0x2030u
#define SH0_SECR        0x2040u
#define SH0_IPR         0x2068u
#define SH0_ICR         0x2070u
#define PARAM(n)        (TPCC + 0x4000u + 32u * (n))
#define EV_RX           3u
#define TPCC_EVT_MUX_0_3 0xf90u
#define CM_PER_TPCC     0x0bcu
#define CM_PER_TPTC1    0x0fcu

/* Off by default: in the emulator, DMA reads while audio runs cost audio
 * repeats (ISR stretched to ~8 ms; cause not found), and a polled DMA
 * transfer frees no CPU anyway. EXTRA=-DMMC_DMA turns it on. */
#ifdef MMC_DMA
int mmc_use_dma = 1;
#else
int mmc_use_dma = 0;
#endif
static int dma_ready;

const char *mmc_error;
static int ready_of[MMC_UNITS], hc_of[MMC_UNITS];
static uint32_t rca_of[MMC_UNITS];
static int ready, high_capacity, cur = -1;
static uint32_t rca;

static void select(int unit)
{
    if (cur >= 0) {
        ready_of[cur] = ready;
        hc_of[cur] = high_capacity;
        rca_of[cur] = rca;
    }
    cur = unit;
    base = base_of[unit];
    ready = ready_of[unit];
    high_capacity = hc_of[unit];
    rca = rca_of[unit];
}

static int fail(const char *why)
{
    mmc_error = why;
    return 0;
}

static int wait_clear(uint32_t reg, uint32_t bits)
{
    uint32_t t0 = time_us();

    while (REG(base + reg) & bits) {
        if (time_us() - t0 > TIMEOUT_US) {
            return 0;
        }
    }
    return 1;
}

/* Waits for any of the bits in STAT; 0 on an error or timeout. Every card
 * command and block passes here, so long card work kicks WDT1 here. */
static int wait_stat(uint32_t bits)
{
    uint32_t t0 = time_us();

    wdt_kick();
    for (;;) {
        uint32_t st = REG(base + STAT);
        if (st & STAT_ERRI) {
            return 0;
        }
        if (st & bits) {
            return 1;
        }
        if (time_us() - t0 > TIMEOUT_US) {
            return 0;
        }
    }
}

static void reset_lines(uint32_t bits)
{
    REG(base + SYSCTL) |= bits;
    wait_clear(SYSCTL, bits);
}

static int command(unsigned index, uint32_t arg, uint32_t flags)
{
    if (!wait_clear(PSTATE, PSTATE_CMDI)) {
        return 0;
    }
    REG(base + STAT) = 0xffffffffu;
    REG(base + ARG) = arg;
    REG(base + CMD) = CMD_INDEX(index) | flags;
    if (!wait_stat(STAT_CC)) {
        REG(base + STAT) = 0xffffffffu;
        reset_lines(SYSCTL_SRC);
        return 0;
    }
    REG(base + STAT) = STAT_CC;
    return 1;
}

static int app_command(unsigned index, uint32_t arg, uint32_t flags)
{
    return command(55, rca << 16, RSP_48) && command(index, arg, flags);
}

/* Front card sector cache, direct-mapped and write-through: FatFs re-reads
 * a directory's sectors on every open by name, so opening each file of a
 * big folder would otherwise cost O(files^2) card reads. Cleared whenever
 * the card is (re)initialised or found gone. */
#define CACHE_LINES 2048u

static uint32_t cache_tag[CACHE_LINES];     /* lba + 1, 0 = empty */
static uint8_t cache_data[CACHE_LINES][512] __attribute__((aligned(4)));

static void cache_clear(int unit)
{
    if (unit == MMC_FRONT) {
        memset(cache_tag, 0, sizeof(cache_tag));
    }
}

static void cache_put(int unit, uint32_t lba, const uint8_t *buf)
{
    if (unit == MMC_FRONT) {
        cache_tag[lba % CACHE_LINES] = lba + 1;
        memcpy(cache_data[lba % CACHE_LINES], buf, 512);
    }
}

int mmc_init(int unit)
{
    select(unit);
    cache_clear(unit);
    ready = 0;
    mmc_error = 0;
    if (!clock_enable(CM_PER, clk_of[unit], CLK_ENABLE)) {
        return fail("SD CLOCK");
    }
    REG(base + SYSCONFIG) = 0x2;
    for (uint32_t t0 = time_us(); !(REG(base + SYSSTATUS) & 1u);) {
        if (time_us() - t0 > TIMEOUT_US) {
            return fail("SD RESET");
        }
    }
    REG(base + SYSCTL) = 0x01000000u;          /* SRA */
    wait_clear(SYSCTL, 0x01000000u);
    REG(base + CAPA) = REG(base + CAPA) | 0x07000000u;     /* VS18/30/33 */
    REG(base + SYSCONFIG) = 0x1;
    REG(base + CON) = 0;
    REG(base + HCTL) = HCTL_3V0;
    REG(base + HCTL) = HCTL_3V0 | HCTL_SDBP;
    REG(base + SYSCTL) = SYSCTL_400K;
    REG(base + IE) = 0xffffffffu;              /* status bits (no interrupt lines) */

    /* 80 clocks of initialization stream. */
    REG(base + CON) = CON_INIT;
    REG(base + CMD) = 0;
    {
        uint32_t t0 = time_us();
        while (time_us() - t0 < 2000u) {
        }
    }
    REG(base + CON) = 0;
    REG(base + STAT) = 0xffffffffu;

    rca = 0;
    if (!command(0, 0, RSP_NONE)) {
        return fail("NO SD CARD");
    }
    int v2 = command(8, 0x1aa, RSP_48) && (REG(base + RSP10) & 0xfff) == 0x1aa;
    uint32_t t0 = time_us(), ocr = 0;
    do {
        if (!app_command(41, v2 ? 0x40ff8000u : 0x00ff8000u, RSP_48)) {
            return fail("SD ACMD41");
        }
        ocr = REG(base + RSP10);
        if (time_us() - t0 > 1000000u) {
            return fail("SD NOT READY");
        }
    } while (!(ocr & 0x80000000u));
    high_capacity = (ocr & 0x40000000u) != 0;
    if (!command(2, 0, RSP_136) || !command(3, 0, RSP_48)) {
        return fail("SD CMD2/3");
    }
    rca = REG(base + RSP10) >> 16;
    if (!command(7, rca << 16, RSP_48B) || !wait_stat(STAT_TC)) {
        return fail("SD SELECT");
    }
    REG(base + STAT) = STAT_TC;
    if (!command(16, 512, RSP_48)) {
        return fail("SD BLOCKLEN");
    }
    if (!app_command(6, 2, RSP_48)) {
        return fail("SD BUS WIDTH");
    }
    REG(base + HCTL) = HCTL_3V0 | HCTL_SDBP | HCTL_DTW4;
    REG(base + SYSCTL) = SYSCTL_24M & ~0x4u;   /* clock off while changing */
    REG(base + SYSCTL) = SYSCTL_24M;
    ready = 1;
    ready_of[unit] = 1;
    return 1;
}

int mmc_ready(int unit)
{
    select(unit);
    return ready;
}

int mmc_present(int unit)
{
    select(unit);
    if (ready && !command(13, rca << 16, RSP_48)) {
        ready = 0;
        cache_clear(unit);
    }
    return ready;
}

static uint32_t address(uint32_t lba)
{
    return high_capacity ? lba : lba * 512u;
}

/* Card back in the transfer state after a write (CMD13 until ready). */
static int wait_card_ready(void)
{
    uint32_t t0 = time_us();

    for (;;) {
        if (!command(13, rca << 16, RSP_48)) {
            return 0;
        }
        uint32_t st = REG(base + RSP10);
        if ((st & 0x100u) && ((st >> 9) & 0xfu) == 4u) {
            return 1;
        }
        if (time_us() - t0 > TIMEOUT_US) {
            return 0;
        }
    }
}

static int dma_setup(void)
{
    if (!dma_ready) {
        if (!clock_enable(CM_PER, CM_PER_TPCC, CLK_ENABLE) ||
            !clock_enable(CM_PER, CM_PER_TPTC1, CLK_ENABLE)) {
            return 0;
        }
        REG(CONTROL + TPCC_EVT_MUX_0_3) = 0x02010000u;     /* event 2 = SDTXEVT2, 3 = SDRXEVT2 */
        REG(TPCC + DCHMAP(EV_RX)) = 32u * EV_RX;
        REG(TPCC + DMAQNUM0) = (REG(TPCC + DMAQNUM0) & ~(7u << (4 * EV_RX))) | (1u << (4 * EV_RX));
        REG(TPCC + DRAE0) |= 1u << EV_RX;
        dma_ready = 1;
    }
    return 1;
}

static void dma_stop(void)
{
    REG(TPCC + SH0_EECR) = 1u << EV_RX;
    REG(TPCC + SH0_ECR) = 1u << EV_RX;
    REG(TPCC + SH0_SECR) = 1u << EV_RX;
    REG(TPCC + EMCR) = 1u << EV_RX;
    REG(TPCC + SH0_ICR) = 1u << EV_RX;
}

static void dma_arm(uint8_t *buf, uint32_t count)
{
    volatile uint32_t *p = (volatile uint32_t *)PARAM(EV_RX);

    dma_stop();
    p[0] = (EV_RX << 12) | (1u << 20) | (1u << 2);     /* TCC, TCINTEN, AB-synchronized */
    p[1] = base + DATA;
    p[2] = 4u | (128u << 16);                           /* one block per event */
    p[3] = (uint32_t)buf;
    p[4] = 4u << 16;                                    /* SRCBIDX 0, DSTBIDX 4 */
    p[5] = 0xffffu;
    p[6] = 512u << 16;                                  /* SRCCIDX 0, DSTCIDX 512 */
    p[7] = count;
    REG(TPCC + SH0_EESR) = 1u << EV_RX;
}

static int wait_dma(uint32_t timeout)
{
    for (uint32_t t0 = time_us(); !(REG(TPCC + SH0_IPR) & (1u << EV_RX));) {
        if (time_us() - t0 > timeout) {
            return 0;
        }
    }
    return 1;
}

/* One CMD18 for count (<= BULK_MAX) blocks, bypassing the sector cache
 * (which, being write-through, never holds anything newer than the card). */
static int read_bulk(uint32_t lba, uint8_t *buf, uint32_t count)
{
    uint32_t a = (uint32_t)buf, bytes = count * 512u;
    uint32_t timeout = TIMEOUT_US + count * 1000u;
    int dma = mmc_use_dma && cur == MMC_FRONT && !(a & (CACHE_LINE - 1u)) &&
              a >= 0x80000000u && a + bytes <= DMA_REGION && dma_setup();
    uint32_t flags = RSP_48 | DP | DDIR_READ | CMD_MSBS | CMD_BCE | CMD_ACMD12;

    REG(base + BLK) = 512u | (count << 16);
    if (dma) {
        dcache_flush_range(buf, bytes);
        dma_arm(buf, count);
        flags |= CMD_DE;
    }
    if (!command(18, address(lba), flags)) {
        goto err;
    }
    if (dma) {
        uint32_t t0 = time_us();
        while (!(REG(base + STAT) & (STAT_TC | STAT_ERRI))) {
            if (time_us() - t0 > timeout) {
                goto err;
            }
        }
        if ((REG(base + STAT) & STAT_ERRI) || !wait_dma(TIMEOUT_US)) {
            goto err;
        }
        dma_stop();
        dcache_inval_range(buf, bytes);
    } else {
        for (uint32_t b = 0; b < count; b++, buf += 512) {
            if (!wait_stat(STAT_BRR)) {
                goto err;
            }
            REG(base + STAT) = STAT_BRR;
            for (int i = 0; i < 128; i++) {
                uint32_t w = REG(base + DATA);
                buf[4 * i] = (uint8_t)w;
                buf[4 * i + 1] = (uint8_t)(w >> 8);
                buf[4 * i + 2] = (uint8_t)(w >> 16);
                buf[4 * i + 3] = (uint8_t)(w >> 24);
            }
        }
        if (!wait_stat(STAT_TC)) {
            goto err;
        }
    }
    REG(base + STAT) = STAT_TC;
    return 1;
err:
    if (dma) {
        dma_stop();
        dcache_inval_range((void *)a, bytes);
    }
    REG(base + STAT) = 0xffffffffu;
    reset_lines(SYSCTL_SRC | SYSCTL_SRD);
    command(12, 0, RSP_48B);
    REG(base + STAT) = 0xffffffffu;
    return fail("SD READ");
}

int mmc_read(int unit, uint32_t lba, uint8_t *buf, uint32_t count)
{
    wdt_kick();     /* sector cache hits skip wait_stat */
    select(unit);
    if (!ready) {
        return 0;
    }
    if (count >= BULK_MIN) {
        while (count) {
            uint32_t n = count < BULK_MAX ? count : BULK_MAX;
            if (!read_bulk(lba, buf, n)) {
                /* Untested on hardware: a failed DMA read retries by CPU, and DMA stays off. */
                if (!mmc_use_dma || unit != MMC_FRONT) {
                    return 0;
                }
                mmc_use_dma = 0;
                if (!read_bulk(lba, buf, n)) {
                    return 0;
                }
                mmc_error = 0;
            }
            lba += n;
            buf += n * 512u;
            count -= n;
        }
        return 1;
    }
    for (uint32_t b = 0; b < count; b++, buf += 512) {
        if (unit == MMC_FRONT && cache_tag[(lba + b) % CACHE_LINES] == lba + b + 1) {
            memcpy(buf, cache_data[(lba + b) % CACHE_LINES], 512);
            continue;
        }
        REG(base + BLK) = 512u | (1u << 16);
        if (!command(17, address(lba + b), RSP_48 | DP | DDIR_READ) || !wait_stat(STAT_BRR)) {
            reset_lines(SYSCTL_SRC | SYSCTL_SRD);
            return fail("SD READ");
        }
        REG(base + STAT) = STAT_BRR;
        for (int i = 0; i < 128; i++) {
            uint32_t w = REG(base + DATA);
            buf[4 * i] = (uint8_t)w;
            buf[4 * i + 1] = (uint8_t)(w >> 8);
            buf[4 * i + 2] = (uint8_t)(w >> 16);
            buf[4 * i + 3] = (uint8_t)(w >> 24);
        }
        if (!wait_stat(STAT_TC)) {
            reset_lines(SYSCTL_SRC | SYSCTL_SRD);
            return fail("SD READ END");
        }
        REG(base + STAT) = STAT_TC;
        cache_put(unit, lba + b, buf);
    }
    return 1;
}

int mmc_write(int unit, uint32_t lba, const uint8_t *buf, uint32_t count)
{
    select(unit);
    if (unit == MMC_SOM && !mmc_som_writable) {
        return fail("SOM CARD READ ONLY");
    }
    if (!ready) {
        return 0;
    }
    for (uint32_t b = 0; b < count; b++, buf += 512) {
        REG(base + BLK) = 512u | (1u << 16);
        if (!command(24, address(lba + b), RSP_48 | DP) || !wait_stat(STAT_BWR)) {
            reset_lines(SYSCTL_SRC | SYSCTL_SRD);
            cache_clear(unit);
            return fail("SD WRITE");
        }
        REG(base + STAT) = STAT_BWR;
        for (int i = 0; i < 128; i++) {
            REG(base + DATA) = (uint32_t)buf[4 * i] | ((uint32_t)buf[4 * i + 1] << 8) |
                               ((uint32_t)buf[4 * i + 2] << 16) | ((uint32_t)buf[4 * i + 3] << 24);
        }
        if (!wait_stat(STAT_TC)) {
            reset_lines(SYSCTL_SRC | SYSCTL_SRD);
            cache_clear(unit);
            return fail("SD WRITE END");
        }
        REG(base + STAT) = STAT_TC;
        if (!wait_card_ready()) {
            cache_clear(unit);
            return fail("SD BUSY");
        }
        cache_put(unit, lba + b, buf);
    }
    return 1;
}
