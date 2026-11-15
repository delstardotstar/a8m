/*
 * CV inputs: two MAX11128 ADCs, one on each of McSPI0/1, 16-bit frames at
 * 16 MHz, chip select dropped between words (multi-channel master mode).
 * Setup words are stock's (progress.md, hardware map), except that the
 * SampleSet mode word also sets CHAN_ID, so each result names its channel
 * and the scan's pipeline offset does not matter. Each ADC scans channels
 * 0-7, 0-3, 8-11: 0-3 twice (CV A, as stock's 96 kHz), 4-11 once.
 */
#include "cv.h"
#include "cvframe.h"
#include "audio.h"
#include "hw.h"
#include "mmu.h"
#include "diag.h"
#include "cal.h"
#include "systime.h"

#include <arm_neon.h>

#define MCSPI0          0x48030000u
#define MCSPI1          0x481a0000u
#define CM_PER_SPI0     0x04cu
#define CM_PER_SPI1     0x050u
#define SYSCONFIG       0x110u
#define SYSSTATUS       0x114u
#define MODULCTRL       0x128u
#define MODULCTRL_SINGLE 0x1u
#define CH0CONF         0x12cu
#define CH0STAT         0x130u
#define CH0CTRL         0x134u
#define TX0             0x138u
#define RX0             0x13cu
#define STAT_RXS        0x1u
#define CONF_16BIT      0x200107cbu     /* stock: CLKG, DPE0, WL 16, CS low, /3, mode 3 */
#define CONF_8BIT_FORCE 0x201103cbu     /* stock: 8-bit words, CS held (sequence load) */
#define CONF_SCAN       0x220107cbu     /* stock's streaming CS-to-clock delay (TCS 1), no DMA/FIFO */
#define CONF_DMA        (CONF_SCAN | (1u << 14) | (1u << 15))   /* + DMAW, DMAR */
#define SPI_TIMEOUT     200u            /* reads; a word takes ~1 us */
#define CV_GIVE_UP      16              /* consecutive failed scans */

/* MAX11128 words: reset, config, unipolar, bipolar, SampleSet + CHAN_ID,
 * SampleSet length 16, then the sequence nibbles as 8-bit words. */
static const uint16_t setup_words[] = { 0x0040, 0x8000, 0x8800, 0x9000, 0x4804, 0xb078 };
static const uint8_t sequence[] = { 0x01, 0x23, 0x45, 0x67, 0x01, 0x23, 0x89, 0xab };
static const uint8_t scan_order[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 8, 9, 10, 11 };

volatile float cv_volts[CV_INPUTS];
volatile uint16_t cv_raw[2][16];
volatile int cv_chan_id_ok;
volatile uint8_t cv_row_rot[2];
volatile uint16_t cv_row_bad[2];
volatile uint32_t cv_timeouts, cv_resyncs;
volatile int cv_d2_input;
volatile float cv_d2_volts;
volatile int cv_sums_on;
volatile int32_t cv_sums[CV_INPUTS];
volatile uint32_t cv_sums_epoch;
const char *cv_warning;
static int ready, dma;
static struct cv_rowmap rmap[2];
static int16_t last_scan[2][16];


static const uint32_t spi[2] = { MCSPI0, MCSPI1 };

static int xfer_both(uint32_t tx, uint32_t rx[2])
{
    int ok = 1;

    REG(MCSPI0 + TX0) = tx;
    REG(MCSPI1 + TX0) = tx;
    for (int i = 0; i < 2; i++) {
        uint32_t n = 0;
        while (!(REG(spi[i] + CH0STAT) & STAT_RXS) && ++n <= SPI_TIMEOUT) {
        }
        if (n > SPI_TIMEOUT) {
            ok = 0;
        }
        rx[i] = REG(spi[i] + RX0);  /* also drains a late word */
    }
    return ok;
}

static void conf_both(uint32_t conf)
{
    for (int i = 0; i < 2; i++) {
        REG(spi[i] + CH0CTRL) = 0;
        REG(spi[i] + CH0CONF) = conf;
        REG(spi[i] + CH0CTRL) = 1;
    }
}

/* Streaming (as stock): per ADC an EDMA TX channel feeds the "continue"
 * word to TX0 on every request and an RX channel moves each result into a
 * ring, both reloading themselves forever. The RX ring is transposed: row
 * b holds word b of every scan (BIDX one row, CIDX back to the next
 * column), so one input's samples are contiguous; the RX PaRAM's CCNT says
 * how many scans of this pass are complete. Channel IDs map rows to
 * inputs (CV A has two rows). */
#define TPCC            0x49000000u
#define CM_PER_TPCC     0x0bcu
#define CM_PER_TPTC0    0x024u
#define DCHMAP(ch)      (0x100u + 4u * (ch))
#define DMAQNUM(n)      (0x240u + 4u * (n))
#define DRAE0           0x340u
#define DRAEH0          0x344u
#define EMCR            0x308u
#define EMCRH           0x30cu
#define SH0_ECR         0x2008u
#define SH0_ECRH        0x200cu
#define SH0_EECR        0x2028u
#define SH0_EECRH       0x202cu
#define SH0_EESR        0x2030u
#define SH0_EESRH       0x2034u
#define SH0_SECR        0x2040u
#define SH0_SECRH       0x2044u
#define PARAM(n)        (TPCC + 0x4000u + 32u * (n))
#define SET_RELOAD      70u             /* 70-73; audio uses 64-69 */
#define DMA_STALL_POLLS (64 * BLOCKS_PER_32)

static const uint8_t ev_tx[2] = { 16, 42 }, ev_rx[2] = { 17, 43 };
#define SCANS           128     /* ring length: ~2.7 ms at 48 kHz */
static uint16_t dma_zero __attribute__((section(".dma"), aligned(4)));
static volatile uint16_t ring[2][16][SCANS] __attribute__((section(".dma"), aligned(64)));

static void param(unsigned set, uint32_t src, uint32_t dst, int rx, unsigned link)
{
    volatile uint32_t *p = (volatile uint32_t *)PARAM(set);

    p[0] = 0;                               /* A-synchronized, no interrupt */
    p[1] = src;
    p[2] = 2u | (16u << 16);                /* 16 x 2-byte words a scan */
    p[3] = dst;
    p[4] = rx ? (2u * SCANS) << 16 : 0;
    p[5] = ((0x4000u + 32u * link) & 0xffffu) | (16u << 16);
    p[6] = rx ? (uint32_t)(uint16_t)(int16_t)(2 - 15 * 2 * SCANS) << 16 : 0;
    p[7] = rx ? SCANS : 1;
}

static void dma_events(int on)
{
    uint32_t lo = (1u << ev_tx[0]) | (1u << ev_rx[0]);
    uint32_t hi = (1u << (ev_tx[1] - 32)) | (1u << (ev_rx[1] - 32));

    REG(TPCC + (on ? SH0_EESR : SH0_EECR)) = lo;
    REG(TPCC + (on ? SH0_EESRH : SH0_EECRH)) = hi;
}

static void dma_start(void)
{
    uint32_t lo = (1u << ev_tx[0]) | (1u << ev_rx[0]);
    uint32_t hi = (1u << (ev_tx[1] - 32)) | (1u << (ev_rx[1] - 32));

    clock_require(CM_PER, CM_PER_TPCC, CLK_ENABLE);
    clock_require(CM_PER, CM_PER_TPTC0, CLK_ENABLE);
    dma_zero = 0;
    for (int a = 0; a < 2; a++) {
        unsigned rt = SET_RELOAD + 2u * (unsigned)a, rr = rt + 1u;
        for (int b = 0; b < 16; b++) {
            for (int c = 0; c < SCANS; c++) {
                ring[a][b][c] = 0xffffu;
            }
        }
        param(ev_tx[a], (uint32_t)&dma_zero, spi[a] + TX0, 0, rt);
        param(rt, (uint32_t)&dma_zero, spi[a] + TX0, 0, rt);
        param(ev_rx[a], spi[a] + RX0, (uint32_t)ring[a], 1, rr);
        param(rr, spi[a] + RX0, (uint32_t)ring[a], 1, rr);
        REG(TPCC + DCHMAP(ev_tx[a])) = 32u * ev_tx[a];
        REG(TPCC + DCHMAP(ev_rx[a])) = 32u * ev_rx[a];
    }
    /* Queue 0, as stock: channels 16/17 in DMAQNUM2, 42/43 in DMAQNUM5. */
    REG(TPCC + DMAQNUM(2)) &= ~0x77u;
    REG(TPCC + DMAQNUM(5)) &= ~0x7700u;
    REG(TPCC + DRAE0) |= lo;
    REG(TPCC + DRAEH0) |= hi;
    REG(TPCC + SH0_ECR) = lo;
    REG(TPCC + SH0_ECRH) = hi;
    REG(TPCC + SH0_SECR) = lo;
    REG(TPCC + SH0_SECRH) = hi;
    REG(TPCC + EMCR) = lo;
    REG(TPCC + EMCRH) = hi;
    dma_events(1);
    conf_both(CONF_DMA);
    dma = 1;
}

/* Back to polling if the RX DMA stops moving (its PaRAM's destination
 * stays put for DMA_STALL_POLLS polls). */
static int dma_alive(void)
{
    static uint32_t last_dst;
    static unsigned age;
    uint32_t dst = REG(PARAM(ev_rx[0]) + 12u);

    if (dst != last_dst) {
        last_dst = dst;
        age = 0;
        return 1;
    }
    if (++age < DMA_STALL_POLLS) {
        return 1;
    }
    dma_events(0);
    conf_both(CONF_SCAN);
    dma = 0;
    cv_warning = "CV DMA STALLED: POLLING";
    return 0;
}

void cv_init(void)
{
    uint32_t rx[2];

    for (int a = 0; a < 2; a++) {
        cv_rowmap_init(&rmap[a]);
        for (int p = 0; p < 16; p++) {
            last_scan[a][p] = 2048 << 3;
        }
    }
    clock_require(CM_PER, CM_PER_SPI0, CLK_ENABLE);
    clock_require(CM_PER, CM_PER_SPI1, CLK_ENABLE);
    for (int i = 0; i < 2; i++) {
        REG(spi[i] + SYSCONFIG) = 0x2;
        for (uint32_t n = 0; !(REG(spi[i] + SYSSTATUS) & 1u); n++) {
            if (n > CLOCK_TIMEOUT) {
                cv_warning = "CV SPI RESET TIMEOUT";
                return;
            }
        }
        REG(spi[i] + MODULCTRL) = 0;
    }
    conf_both(CONF_16BIT);
    for (unsigned w = 0; w < sizeof(setup_words) / sizeof(setup_words[0]); w++) {
        if (!xfer_both(setup_words[w], rx)) {
            cv_warning = "CV ADC NO RESPONSE";
            return;
        }
    }
    /* FORCE (chip select held for the whole pattern) only works in
     * single-channel mode; stock switches MODULCTRL around it the same way. */
    for (int i = 0; i < 2; i++) {
        REG(spi[i] + MODULCTRL) = MODULCTRL_SINGLE;
    }
    conf_both(CONF_8BIT_FORCE);
    for (unsigned b = 0; b < sizeof(sequence); b++) {
        xfer_both(sequence[b], rx);
    }
    for (int i = 0; i < 2; i++) {
        REG(spi[i] + MODULCTRL) = 0;
    }
    conf_both(CONF_SCAN);
    ready = 1;
    dma_start();
}

#define UNITS_PER_VOLT      6553.6f

/* Per-frame CV. The ADCs' scan clock is not the audio clock, so each ADC
 * keeps a fractional read position in scans (rd, the block's first frame)
 * that advances by its measured scans per frame (rate) and is pulled
 * towards cv_read_lag scans behind the newest at the start of the block now
 * playing (audio_block_ticks; the interrupt only runs later, so it never
 * finds fewer, and its latency does not move the reads). Each frame takes
 * the whole scan nearest its position; rmap says which ring row holds
 * which scan position. */
/* Scans behind: the nearest scan of the block's last frame + the scan
 * count's step. Raised when reads catch up with unwritten scans (hardware
 * delivers scans in bursts the emulator doesn't). */
#define READ_LAG_MIN    4
#define READ_LAG_MAX    48
#define READ_AHEAD      2
#define BLOCK_SCANS     CV_RING_SPAN
#define RATE_WARMUP     (100 * BLOCKS_PER_32)  /* blocks of running mean before the slow filter */
#define RATE_SLOW       (0.01f / BLOCKS_PER_32)
#define PULL            (0.05f / BLOCKS_PER_32)
volatile uint32_t cv_read_lag = READ_LAG_MIN;
volatile float cv_scan_rate;
volatile int32_t cv_dev_min;
static float rd[2], rate[2] = { 1.0f, 1.0f };
static unsigned rate_n[2];
static unsigned poll_epoch;
static unsigned frames_epoch[CV_INPUTS];
static float frames_buf[CV_INPUTS][AUDIO_BLOCK] __attribute__((aligned(16)));
static struct cv_adc_state hb_state[2];
/* Per ADC and group A/B/C: every frame's values of its four inputs. */
static int16_t cv_q[2][3][AUDIO_BLOCK][4] __attribute__((aligned(16)));

static void map_from(int a, const uint32_t w[16])
{
    uint16_t v[16];

    for (int b = 0; b < 16; b++) {
        v[b] = (uint16_t)w[b];
    }
    cv_rowmap_update(&rmap[a], v);
    cv_row_rot[a] = rmap[a].rot;
    cv_row_bad[a] = rmap[a].bad;
}

/* The scans ring_block reads this block: s0..s0+n-1 (mod SCANS), frame f at
 * ((q0 + f * dq) >> 16) - s0. */
struct span {
    uint32_t q0, dq;
    unsigned s0, n;
};

static struct span block_span(int a)
{
    struct span p;

    p.q0 = (uint32_t)((rd[a] + 0.5f) * 65536.0f);
    p.dq = (uint32_t)(rate[a] * 65536.0f);
    p.s0 = p.q0 >> 16;
    p.n = ((p.q0 + (AUDIO_BLOCK - 1) * p.dq) >> 16) - p.s0 + 1;
    p.n = p.n > BLOCK_SCANS ? BLOCK_SCANS : p.n;
    return p;
}

/* The ring is write-through cached: before reading what the DMA wrote,
 * every row's lines holding the block's span are invalidated (after the
 * scan count was read), then preloaded. */
static void ring_fresh(struct span p, int a)
{
    unsigned lines = 0;

#ifdef TEST_STALE
    return;     /* EXTRA=-DTEST_STALE: emu/plugin/uncached.c must halt */
#endif

    for (unsigned c = p.s0 / 16; c <= (p.s0 + p.n - 1) / 16; c++) {
        lines |= 1u << (c % (SCANS / 16) / 2);
    }
    for (int b = 0; b < 16; b++) {
        for (unsigned l = 0; l < SCANS * 2 / CACHE_LINE; l++) {
            if ((lines >> l) & 1) {
                dcache_inval_lines(&ring[a][b][l * CACHE_LINE / 2], CACHE_LINE);
            }
        }
    }
    dcache_sync();
    for (int b = 0; b < 16; b++) {
        for (unsigned l = 0; l < SCANS * 2 / CACHE_LINE; l++) {
            if ((lines >> l) & 1) {
                preload_lines(&ring[a][b][l * CACHE_LINE / 2], CACHE_LINE);
            }
        }
    }
}

static void dma_scans(uint32_t rx[2][16])
{
    static unsigned prev[2];

    for (int a = 0; a < 2; a++) {
        float late = (float)(int32_t)(time_ticks() - audio_block_ticks()) * (1.0f / AUDIO_TICKS_PER_FRAME);
        unsigned left = REG(PARAM(ev_rx[a]) + 28u) & 0xffffu;
        unsigned done = left && left <= SCANS ? SCANS - left : 0;
        unsigned n = (done + SCANS - prev[a]) % SCANS;

        prev[a] = done;
        /* A running mean first: from the 1.0 guess, a slow filter lets the
         * reads drift towards unwritten scans for seconds when the ADCs
         * scan slower than 48 kHz, and each near miss raised the lag. */
        if (n >= AUDIO_BLOCK / 2 && n <= AUDIO_BLOCK * 2) {
            rate_n[a] += rate_n[a] < RATE_WARMUP;
            rate[a] += ((float)n / AUDIO_BLOCK - rate[a]) * (rate_n[a] < RATE_WARMUP ? 1.0f / (float)rate_n[a] : RATE_SLOW);
        }
        rd[a] += rate[a] * AUDIO_BLOCK;
        late = late < 0.0f ? 0.0f : late > 64.0f ? 64.0f : late;
        /* The oldest read must stay inside the ring. */
        float late_max = (float)(SCANS - 16 - AUDIO_BLOCK) - (float)cv_read_lag;
        late = late > late_max ? late_max : late;
        float want = (float)done - rate[a] * (late + AUDIO_BLOCK) - (float)cv_read_lag;
        float err = want - rd[a];
        err -= SCANS * (float)(int)(err / SCANS);
        err = err > SCANS / 2 ? err - SCANS : err < -SCANS / 2 ? err + SCANS : err;
        int ahead = err + rate[a] * late < (float)READ_AHEAD - (float)cv_read_lag;
        /* Three quarters of a block or more: a block rendered twice or not
         * at all (the read position jumps a block), not the scans' timing. */
        float jump = rate[a] * (AUDIO_BLOCK * 3 / 4);
        int lost = err > jump || err < -jump;
        if (rate_n[a] >= RATE_WARMUP && (int32_t)(err + rate[a] * late) < cv_dev_min) {
            cv_dev_min = (int32_t)(err + rate[a] * late);
        }
        if (lost || ahead) {
            if (ahead && !lost && cv_read_lag < READ_LAG_MAX) {
                cv_read_lag += 2;
                want -= 2.0f;
            }
            rd[a] = want;   /* start, a lost block, or about to read unwritten scans */
            cv_resyncs++;
        } else {
            rd[a] += PULL * err;
        }
        if (rd[a] < 0.0f) {
            rd[a] += SCANS;
        } else if (rd[a] >= SCANS) {
            rd[a] -= SCANS;
        }
        cv_scan_rate = rate[0];
        /* The words of the newest scan the block reads (the row map). */
        struct span p = block_span(a);
        unsigned last = (p.s0 + p.n - 1u) % SCANS;
        ring_fresh(p, a);
        for (int b = 0; b < 16; b++) {
            rx[a][b] = ring[a][b][last];
        }
        map_from(a, rx[a]);
    }
}

/* All twelve inputs of ADC a over the block from the ring, one scan per
 * frame. */
static void ring_block(int a)
{
    struct span p = block_span(a);
    const uint16_t *row[16];
    int16_t r[AUDIO_BLOCK][16] __attribute__((aligned(16)));

    /* Write-through cacheable rows, made fresh by ring_fresh: NEON loads are fine. */
    for (int q = 0; q < 16; q++) {
        row[q] = (const uint16_t *)ring[a][rmap[a].row[q]];
    }
    cv_ring_frames(row, SCANS, p.s0, p.n, p.q0, p.dq, r, AUDIO_BLOCK);
    cv_adc_frames(&hb_state[a], &cal_live.cv_off[a * 4], &cal_live.cv_gain[a * 4], (const int16_t (*)[16])r, AUDIO_BLOCK,
                  cv_q[a]);
}

/* One scan of ADC a's words held over the block (positions by the row map;
 * a missing word keeps its position's last code). */
static void scan_block(int a, const uint32_t w[16])
{
    int16_t (*o)[AUDIO_BLOCK][4] = cv_q[a];

    for (int p = 0; p < 16; p++) {
        uint32_t v = w[rmap[a].row[p]];
        if (v != 0xffffu) {
            last_scan[a][p] = (int16_t)((v & 0xfffu) << 3);
        }
    }
    cv_adc_frames(&hb_state[a], &cal_live.cv_off[a * 4], &cal_live.cv_gain[a * 4], (const int16_t (*)[16])last_scan[a], 1, o);
    for (int g = 0; g < 3; g++) {
        for (int f = 1; f < AUDIO_BLOCK; f++) {
            vst1_s16(o[g][f], vld1_s16(o[g][0]));
        }
    }
}

/* Input in's value at frame f is q_of(in)[4 * f]. */
static int16_t *q_of(int in)
{
    int n = in / 3;

    return &cv_q[n >> 2][in % 3][0][n & 3];
}

const float *cv_frames(int in)
{
    float *o = frames_buf[in];
    int k = in / 3 & 3;
    const int16_t *q = q_of(in) - k;

    if (frames_epoch[in] == poll_epoch) {
        return o;
    }
    frames_epoch[in] = poll_epoch;
    for (int f = 0; f < AUDIO_BLOCK; f += 8) {
        int16x8x4_t v = vld4q_s16(q + 4 * f);
        int16x8_t x = k == 0 ? v.val[0] : k == 1 ? v.val[1] : k == 2 ? v.val[2] : v.val[3];
        vst1q_f32(o + f, vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(x))), 1.0f / UNITS_PER_VOLT));
        vst1q_f32(o + f + 4, vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(x))), 1.0f / UNITS_PER_VOLT));
    }
    return o;
}

void cv_poll(void)
{
    uint32_t rx[2][16];
    int with_id = 0;

#ifdef PROFILE_VOICE
    /* emu/voicecost.py: a ring of constant (0 V) scans, filled once. */
    if (!ready) {
        static int filled;
        for (int a = 0; a < 2; a++) {
            for (int b = 0; b < 16; b++) {
                for (int c = 0; c < SCANS && !filled; c++) {
                    ring[a][b][c] = (uint16_t)(scan_order[b] << 12 | 0x800);
                }
                rx[a][b] = ring[a][b][0];
            }
            map_from(a, rx[a]);
            rd[a] = (float)(((unsigned)rd[a] + AUDIO_BLOCK) % SCANS);
        }
        filled = 1;
        dma = 1;
        with_id = 1;
        poll_epoch++;
        goto block;
    }
#endif
    if (!ready) {
        return;
    }
    poll_epoch++;
    if (dma && dma_alive()) {
        dma_scans(rx);
        for (int a = 0; a < 2; a++) {
            for (int w = 0; w < 16; w++) {
                with_id |= rx[a][w] != 0xffffu && (rx[a][w] >> 12);
            }
        }
        if (!with_id) {
            return;             /* nothing streamed yet */
        }
        goto block;
    }
    for (int w = 0; w < 16; w++) {
        uint32_t r[2];
        if (!xfer_both(0x0000, r)) {
            static int failed_run;
            cv_timeouts++;
            if (++failed_run >= CV_GIVE_UP) {
                ready = 0;
                cv_warning = "CV SPI TIMEOUTS: OFF";
            }
            return;
        }
        rx[0][w] = r[0];
        rx[1][w] = r[1];
        with_id |= (int)((r[0] | r[1]) >> 12);
    }
    for (int a = 0; a < 2; a++) {
        map_from(a, rx[a]);
    }
block:
    for (int a = 0; a < 2; a++) {
        if (dma) {
            ring_block(a);
        } else {
            scan_block(a, rx[a]);
        }
    }
    cv_chan_id_ok = with_id != 0;
    for (int a = 0; a < 2; a++) {
        for (int p = 0; p < 16; p++) {
            uint32_t v = rx[a][rmap[a].row[p]];
            if (v != 0xffffu) {
                cv_raw[a][scan_order[p]] = (uint16_t)(v & 0xfff);
            }
        }
    }
    int d2 = cv_d2_input - 1;
    if (d2 >= 0 && d2 < CV_INPUTS) {
        float v = cv_d2_volts * UNITS_PER_VOLT;
        int16_t c = (int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)(v + (v < 0 ? -0.5f : 0.5f)));
        int16_t *q = q_of(d2);
        for (int f = 0; f < AUDIO_BLOCK; f++) {
            q[4 * f] = c;
        }
    }
    /* cv_volts[12a + 3k + g] = cv_q[a][g][last frame][k] */
    for (int a = 0; a < 2; a++) {
        float32x4x3_t v;
        for (int g = 0; g < 3; g++) {
            int32x4_t x = vmovl_s16(vld1_s16(cv_q[a][g][AUDIO_BLOCK - 1]));
            v.val[g] = vmulq_n_f32(vcvtq_f32_s32(x), 1.0f / UNITS_PER_VOLT);
        }
        vst3q_f32((float *)&cv_volts[12 * a], v);
    }
    if (cv_sums_on) {
        for (int in = 0; in < CV_INPUTS; in++) {
            const int16_t *q = q_of(in);
            int32_t t = 0;
            for (int f = 0; f < AUDIO_BLOCK; f++) {
                t += q[4 * f];
            }
            cv_sums[in] = t;
        }
        cv_sums_epoch++;
    }
}
