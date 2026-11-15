/*
 * Audio I/O. McASP0 drives the 8 channel outputs: 4 serializers x 2 TDM
 * slots of 32 bits, and it is the clock master (bit clock and frame sync
 * from the external high-frequency clock). McASP1 runs on those clocks for
 * mix L/R out (serializer 0) and the sample input (serializer 1). Register
 * values and the bring-up order are the stock app's, observed in the
 * emulator; the DMA buffering is our own.
 *
 * EDMA: each McASP raises one event per TDM slot. A transfer per event moves
 * one word per active serializer. Each port plays from two half buffers of
 * AUDIO_BLOCK frames whose PaRAM sets link to each other; McASP0's half
 * completes raise the interrupt that renders the half just played.
 */
#include "audio.h"
#include "prof.h"
#include "diag.h"
#include "hw.h"
#include "i2c.h"
#include "intc.h"
#include "mmu.h"
#include "rec.h"
#include "cal.h"
#include "systime.h"

#include <arm_neon.h>
#include <string.h>

#define MCASP0          0x48038000u
#define MCASP1          0x4803c000u
#define MCASP0_DATA     0x46000000u
#define MCASP1_DATA     0x46400000u
#define PFUNC   0x010u
#define PDIR    0x014u
#define GBLCTL  0x044u
#define RGBLCTL 0x060u
#define RMASK   0x064u
#define RFMT    0x068u
#define AFSRCTL 0x06cu
#define ACLKRCTL 0x070u
#define AHCLKRCTL 0x074u
#define RTDM    0x078u
#define RSTAT   0x080u
#define XGBLCTL 0x0a0u
#define XMASK   0x0a4u
#define XFMT    0x0a8u
#define AFSXCTL 0x0acu
#define ACLKXCTL 0x0b0u
#define AHCLKXCTL 0x0b4u
#define XTDM    0x0b8u
#define XSTAT   0x0c0u
#define SRCTL(n) (0x180u + 4u * (n))
#define WFIFOCTL 0x1000u
#define RFIFOCTL 0x1008u
#define GBL_RHCLKRST (1u << 1)
#define GBL_RSRCLR   (1u << 2)
#define GBL_RSMRST   (1u << 3)
#define GBL_RFRST    (1u << 4)
#define GBL_XHCLKRST (1u << 9)
#define GBL_XCLKRST  (1u << 8)
#define GBL_XSRCLR   (1u << 10)
#define GBL_XSMRST   (1u << 11)
#define GBL_XFRST    (1u << 12)
#define CM_PER_MCASP0 0x034u
#define CM_PER_MCASP1 0x068u

/* EDMA3 channel controller, shadow region 0 (its completion IRQ is 12). */
#define TPCC            0x49000000u
#define TPCC_DCHMAP(ch) (0x100u + 4u * (ch))
#define TPCC_DMAQNUM(n) (0x240u + 4u * (n))
#define TPCC_DRAE0      0x340u
#define TPCC_EMR        0x300u
#define TPCC_EMCR       0x308u
#define SH0_ER          0x2000u
#define SH0_ECR         0x2008u
#define SH0_EER         0x2020u
#define SH0_EECR        0x2028u
#define SH0_SER         0x2038u
#define SH0_SECR        0x2040u
#define SH0_EESR        0x2030u
#define SH0_IESR        0x2060u
#define SH0_IPR         0x2068u
#define SH0_ICR         0x2070u
#define SH0_IEVAL       0x2078u
#define PARAM(n)        (TPCC + 0x4000u + 32u * (n))
#define CM_PER_TPCC     0x0bcu
#define CM_PER_TPTC0    0x024u
#define OPT_SYNCDIM_AB  (1u << 2)
#define OPT_TCC(n)      ((uint32_t)(n) << 12)
#define OPT_TCINTEN     (1u << 20)

#define EV_MCASP0_TX    8
#define EV_MCASP1_TX    10
#define EV_MCASP1_RX    11
#define SET_M0_A        64      /* PaRAM reload sets */
#define SET_M1T_A       66
#define SET_M1R_A       68

/* Converter resets on GPIO2_22/23. The converters are reset and configured
 * before any McASP clock starts, as in the stock boot. */
#define CODEC_RESET_A   22
#define CODEC_RESET_B   23
/* Driven high by stock well after both McASPs run: most likely the analog
 * output un-mute (hardware run 9: clocks and DMA ran, outputs silent). */
#define OUTPUT_UNMUTE_PIN 24
#define UNMUTE_AFTER_BLOCKS (64 * BLOCKS_PER_32)
/* GPIO2_10 (input, no pull) selects between two converter setups in the
 * stock app: a board revision strap. Low: writes to 0x10 and 0x18; high
 * (this module): a wait, then writes to 0x10 and 0x13. */
#define BOARD_REV_PIN   10
#define CODEC_DAC_ADDR  0x10
#define CODEC_IN_ADDR   0x18
#define CODEC_B_ADDR_A  0x10
#define CODEC_B_ADDR_B  0x13

/* In the write-through DMA region (mmu.h); zeroed in audio_start. */
#define DMA_BUF __attribute__((section(".dma"), aligned(64)))
static int32_t m0_buf[2][AUDIO_BLOCK][2][4] DMA_BUF;    /* [half][frame][slot][ser] */
/* McASP1 runs from outside clocks at M1_RATIO times McASP0's frame rate
 * (hardware run 14: exactly 4), so its halves hold M1_FRAMES frames. */
#define M1_RATIO        4
#define M1_FRAMES       (AUDIO_BLOCK * M1_RATIO)
static int32_t m1t_buf[2][M1_FRAMES][2] DMA_BUF;
/* The input runs freely through a longer ring (read by position): an
 * interrupt up to 20 ms late loses nothing. */
#define M1R_FRAMES      (16 * 32 * M1_RATIO)
static int32_t m1r_buf[2][M1R_FRAMES][2] DMA_BUF;
static int32_t in[AUDIO_BLOCK][2];
static int32_t out[AUDIO_BLOCK][AUDIO_OUTPUTS], mix[AUDIO_BLOCK][2];
static int32_t mix_ready[AUDIO_BLOCK][2];
static audio_render_fn render;
const char *audio_warning;
int audio_board_rev_b;
static volatile uint32_t blocks;
static unsigned half, last_half = 1;
struct audio_stats audio_stats;

/* 32 bytes into a DMA buffer as two 16-byte bursts. */
static inline void burst8(int32_t *dst, const int32_t *src)
{
    __asm__ volatile("ldm %1, {r4-r7}\n\tstm %0, {r4-r7}\n\t"
                     "ldm %2, {r4-r7}\n\tstm %3, {r4-r7}"
                     : : "r"(dst), "r"(src), "r"(src + 4), "r"(dst + 4) : "r4", "r5", "r6", "r7", "memory");
}

static void delay_spin(volatile uint32_t n)
{
    while (n--) {
    }
}

static void param(unsigned set, uint32_t opt, uint32_t src, uint32_t acnt, uint32_t bcnt,
                  uint32_t dst, int16_t sbidx, int16_t dbidx, unsigned link, int16_t scidx,
                  int16_t dcidx, uint32_t ccnt)
{
    volatile uint32_t *p = (volatile uint32_t *)PARAM(set);

    p[0] = opt;
    p[1] = src;
    p[2] = acnt | (bcnt << 16);
    p[3] = dst;
    p[4] = (uint16_t)sbidx | ((uint32_t)(uint16_t)dbidx << 16);
    p[5] = (0x4000u + 32u * link) & 0xffffu;
    p[6] = (uint16_t)scidx | ((uint32_t)(uint16_t)dcidx << 16);
    p[7] = ccnt;
}

static void param_copy(unsigned to, unsigned from)
{
    for (unsigned i = 0; i < 8; i++) {
        ((volatile uint32_t *)PARAM(to))[i] = ((volatile uint32_t *)PARAM(from))[i];
    }
}

/* Two linked half-buffer sets per port; the channel starts on the first. */
static void edma_setup(void)
{
    const uint32_t m0 = OPT_SYNCDIM_AB | OPT_TCC(EV_MCASP0_TX) | OPT_TCINTEN;
    const uint32_t m1 = OPT_SYNCDIM_AB | OPT_TCC(EV_MCASP1_TX) | OPT_TCINTEN;
    const uint32_t m1r = OPT_SYNCDIM_AB | OPT_TCC(EV_MCASP1_RX);

    clock_require(CM_PER, CM_PER_TPCC, CLK_ENABLE);
    clock_require(CM_PER, CM_PER_TPTC0, CLK_ENABLE);

    /* Per slot event: 4 words (one per serializer); 2 slots x frames. */
    for (unsigned h = 0; h < 2; h++) {
        param(SET_M0_A + h, m0, (uint32_t)m0_buf[h], 4, 4, MCASP0_DATA, 4, 0,
              SET_M0_A + !h, 16, 0, 2 * AUDIO_BLOCK);
        param(SET_M1T_A + h, m1, (uint32_t)m1t_buf[h], 4, 1, MCASP1_DATA, 0, 0,
              SET_M1T_A + !h, 4, 0, 2 * M1_FRAMES);
        param(SET_M1R_A + h, m1r, MCASP1_DATA, 4, 1, (uint32_t)m1r_buf[h], 0, 0,
              SET_M1R_A + !h, 0, 4, 2 * M1R_FRAMES);
    }
    param_copy(EV_MCASP0_TX, SET_M0_A);
    param_copy(EV_MCASP1_TX, SET_M1T_A);
    param_copy(EV_MCASP1_RX, SET_M1R_A);

    const uint32_t chans = (1u << EV_MCASP0_TX) | (1u << EV_MCASP1_TX) | (1u << EV_MCASP1_RX);
    REG(TPCC + TPCC_DCHMAP(EV_MCASP0_TX)) = 32u * EV_MCASP0_TX;
    REG(TPCC + TPCC_DCHMAP(EV_MCASP1_TX)) = 32u * EV_MCASP1_TX;
    REG(TPCC + TPCC_DCHMAP(EV_MCASP1_RX)) = 32u * EV_MCASP1_RX;
    REG(TPCC + TPCC_DMAQNUM(1)) &= ~0x7700u;    /* channels 10, 11 on queue 0 */
    REG(TPCC + TPCC_DMAQNUM(1)) &= ~0x7u;       /* channel 8 on queue 0 */
    REG(TPCC + TPCC_DRAE0) |= chans;
    REG(TPCC + SH0_ECR) = chans;
    REG(TPCC + SH0_SECR) = chans;
    REG(TPCC + TPCC_EMCR) = chans;
    REG(TPCC + SH0_ICR) = (1u << EV_MCASP0_TX) | (1u << EV_MCASP1_TX);
    REG(TPCC + SH0_IESR) = (1u << EV_MCASP0_TX) | (1u << EV_MCASP1_TX);
}

static void edma_enable_events(void)
{
    REG(TPCC + SH0_EESR) = (1u << EV_MCASP0_TX) | (1u << EV_MCASP1_TX) | (1u << EV_MCASP1_RX);
}

static void gbl_set(uint32_t base, uint32_t bits)
{
    REG(base + GBLCTL) |= bits;
    for (uint32_t n = 0; n < CLOCK_TIMEOUT; n++) {
        if ((REG(base + GBLCTL) & bits) == bits) {
            return;
        }
    }
    diag_fail_why(base == MCASP0 ? "MCASP0 GBLCTL" : "MCASP1 GBLCTL", bits, REG(base + GBLCTL));
}

static void mcasp0_config(void)
{
    clock_require(CM_PER, CM_PER_MCASP0, CLK_ENABLE);
    REG(MCASP0 + GBLCTL) = 0;
    REG(MCASP0 + RGBLCTL) = 0;
    REG(MCASP0 + XGBLCTL) = 0;
    REG(MCASP0 + WFIFOCTL) = 0;
    REG(MCASP0 + XMASK) = 0xffffffffu;
    REG(MCASP0 + XFMT) = 0x000180f0u;       /* 32-bit slots, MSB first, 1-bit delay, DMA port */
    REG(MCASP0 + AFSXCTL) = 0x00000113u;    /* 2-slot TDM, word frame sync, internal, inverted */
    REG(MCASP0 + ACLKXCTL) = 0x000000a3u;   /* internal bit clock = AHCLKX / 4, falling edge */
    REG(MCASP0 + AHCLKXCTL) = 0x00000001u;  /* AHCLKX from the pin, divider 2 */
    REG(MCASP0 + XTDM) = 0x3u;
    for (int s = 0; s < 4; s++) {
        REG(MCASP0 + SRCTL(s)) = 0x1u;      /* transmit */
    }
    REG(MCASP0 + PFUNC) = 0xe2000030u;
    REG(MCASP0 + PDIR) = 0x1400000fu;       /* AXR0-3, ACLKX, AFSX out */
}

static void mcasp1_config(void)
{
    clock_require(CM_PER, CM_PER_MCASP1, CLK_ENABLE);
    REG(MCASP1 + GBLCTL) = 0;
    REG(MCASP1 + RGBLCTL) = 0;
    REG(MCASP1 + XGBLCTL) = 0;
    REG(MCASP1 + RFIFOCTL) = 0;
    REG(MCASP1 + WFIFOCTL) = 0;
    REG(MCASP1 + RMASK) = 0xffffffffu;
    REG(MCASP1 + RFMT) = 0x000180f0u;
    REG(MCASP1 + XMASK) = 0xffffffffu;
    REG(MCASP1 + XFMT) = 0x000180f0u;
    REG(MCASP1 + AFSRCTL) = 0x00000111u;    /* external frame sync */
    REG(MCASP1 + AFSXCTL) = 0x00000111u;
    REG(MCASP1 + ACLKRCTL) = 0x00000083u;   /* external bit clock */
    REG(MCASP1 + AHCLKRCTL) = 0;
    REG(MCASP1 + ACLKXCTL) = 0x00000083u;
    REG(MCASP1 + AHCLKXCTL) = 0;
    REG(MCASP1 + RTDM) = 0x3u;
    REG(MCASP1 + XTDM) = 0x3u;
    REG(MCASP1 + SRCTL(0)) = 0x1u;          /* mix out */
    REG(MCASP1 + SRCTL(1)) = 0x2u;          /* sample input */
    REG(MCASP1 + PFUNC) = 0xe200003cu;
    REG(MCASP1 + PDIR) = 0x08000001u;
}

static void codec_init(void)
{
    static const uint8_t dac_a[] = { 0x07, 0x03 };
    static const uint8_t dac_b[] = { 0x81, 0xe9, 0x40, 0x09, 0x00, 0x00, 0x13, 0x02 };
    static const uint8_t in_a[] = { 0x81, 0x80, 0x10, 0x00 };
    static const uint8_t rev_b_10[] = { 0x00, 0x1f, 0x0f, 0x00, 0x38, 0x06, 0x7a, 0x29, 0x04 };
    static const uint8_t rev_b_13[] = { 0x00, 0x0f, 0x22, 0x00, 0xff, 0xff, 0x00, 0x00, 0x03,
                                        0x00, 0x00, 0x0d, 0x0c, 0x00, 0x00, 0x50 };

    audio_board_rev_b = (REG(GPIO2 + GPIO_DATAIN) >> BOARD_REV_PIN) & 1u;
    REG(GPIO2 + GPIO_CLEAR) = (1u << CODEC_RESET_A) | (1u << CODEC_RESET_B);
    REG(GPIO2 + GPIO_OE) &= ~((1u << CODEC_RESET_A) | (1u << CODEC_RESET_B));
    delay_spin(2000000);
    REG(GPIO2 + GPIO_SET) = (1u << CODEC_RESET_A) | (1u << CODEC_RESET_B);
    delay_spin(2000000);

    i2c0_init();
    if (audio_board_rev_b) {
        uint32_t start = time_us();
        while (time_us() - start < 50000u) {
        }
        if (!i2c0_write(CODEC_B_ADDR_A, rev_b_10, sizeof(rev_b_10))) {
            audio_warning = "I2C 0X10 NACK";
        }
        if (!i2c0_write(CODEC_B_ADDR_B, rev_b_13, sizeof(rev_b_13))) {
            audio_warning = "I2C 0X13 NACK";
        }
        return;
    }
    if (!i2c0_write(CODEC_DAC_ADDR, dac_a, sizeof(dac_a))) {
        diag_fail_why("I2C 0X10 WRITE 1", i2c0_sent, i2c0_status);
    }
    if (!i2c0_write(CODEC_DAC_ADDR, dac_b, sizeof(dac_b))) {
        diag_fail_why("I2C 0X10 WRITE 2", i2c0_sent, i2c0_status);
    }
    /* Hardware run 4: 0x18 NACKs after all 4 bytes; stock sends the same
     * bytes and carries on, so this is a warning, not a stop. */
    if (!i2c0_write(CODEC_IN_ADDR, in_a, sizeof(in_a))) {
        audio_warning = "I2C 0X18 NACK";
    }
}

/* The half buffer a port's DMA is not using, from its active PaRAM set's
 * address field (word 1 = SRC, 3 = DST), so late or merged interrupts and
 * the two ports' phase offset cannot make us write the half being played. */
static unsigned idle_half(unsigned set, unsigned word, const void *half1, uint32_t bytes)
{
    uint32_t addr = ((volatile uint32_t *)PARAM(set))[word];

    return addr - (uint32_t)half1 < bytes ? 0 : 1;
}

/* McASP0 completion renders a block into its idle half and keeps the mix
 * block; McASP1 completion copies the latest mix block into its own idle
 * half. The ports' half boundaries can be offset, so each is serviced by
 * its own interrupt; the mix lags the channel outputs by at most a block. */
/* The sample input frames McASP1's receive DMA wrote since the last call
 * (its PaRAM destination is the write position in the two halves). */
static void input_block(void)
{
    static unsigned rd;
    static uint32_t last;
    uint32_t now = time_ticks();
    const unsigned ring = 2u * M1R_FRAMES;
    uint32_t dst = ((volatile uint32_t *)PARAM(EV_MCASP1_RX))[3];
    unsigned wr = ((dst - (uint32_t)m1r_buf) / 8u) % ring, n = (wr - rd) % ring;
    const volatile int32_t (*buf)[2] = (const volatile int32_t (*)[2])m1r_buf;

    if (n > 3u * 32u * M1_RATIO) {
        audio_stats.in_late++;
    }
    /* 24 MHz ticks: longer than the ring means frames were overwritten. */
    if (last && now - last > (2u * M1R_FRAMES - 2u * M1_FRAMES) * 125u) {
        audio_stats.in_lost++;
    }
    last = now;
    do {
        unsigned k = n < ring - rd ? n : ring - rd;
        k = k < M1_FRAMES * 2u ? k : M1_FRAMES * 2u;
        /* Write-through ring: drop what the cache holds of these frames. */
        dcache_inval_lines(buf + rd, k * sizeof(buf[0]));
        dcache_sync();
        preload_lines(buf + rd, k * sizeof(buf[0]));
        rec_input(buf + rd, k, in);
        rd = (rd + k) % ring;
        n -= k;
    } while (n);
}

/* When the half now playing started, in time_ticks: measured from the
 * frames already played, smoothed (the count has half-frame steps). */
static uint32_t block_ticks;
static int block_ticks_ok;

static void block_clock(uint32_t now, uint32_t played_slots)
{
    const uint32_t tpf = AUDIO_TICKS_PER_FRAME;
    uint32_t meas = now - played_slots * (tpf / 2u);
    int32_t err = (int32_t)(meas - (block_ticks + AUDIO_BLOCK * tpf));

    if (!block_ticks_ok || err > (int32_t)(4u * tpf) || err < -(int32_t)(4u * tpf)) {
        block_ticks = meas;
        block_ticks_ok = 1;
    } else {
        block_ticks += AUDIO_BLOCK * tpf + (uint32_t)(err / 8);
    }
}

uint32_t audio_block_ticks(void)
{
    return block_ticks;
}

static void render_block(void)
{
    uint32_t played = 2 * AUDIO_BLOCK - (((volatile uint32_t *)PARAM(EV_MCASP0_TX))[7] & 0xffffu);

    block_clock(time_ticks(), played);

    if (played / 2 > audio_stats.late_max) {
        audio_stats.late_max = played / 2;
    }
    half = idle_half(EV_MCASP0_TX, 1, m0_buf[1], sizeof(m0_buf[1]));
    if (half == last_half) {
        audio_stats.repeats++;
    }
    last_half = half;
    DIAG_IRQ(DT_IRQ_INPUT, 0);
    prof_to(PS_RIN);
    input_block();
    prof_to(PS_REST);
    render(out, mix, (const int32_t (*)[2])in, AUDIO_BLOCK);
    DIAG_IRQ(DT_IRQ_OUTPUT, 0);
    prof_to(PS_ROUT);
    rec_monitor(mix, AUDIO_BLOCK);
    if (cal_mute) {
        memset(out, 0, sizeof(out));
        memset(mix, 0, sizeof(mix));
    }
    /* Stock's output calibration, the last step before the DACs: channel
     * n + byte x 65536, 2 x offset - mix (the mix output stage inverts),
     * both saturating. */
    int32x4x2_t co;
    int32x2_t mo;
    {
        int32_t c[8] __attribute__((aligned(16)));
        for (int n = 0; n < 8; n++) {
            c[n] = (int32_t)cal_live.ch_off[n] * 65536;
        }
        co = vld2q_s32(c);
        for (int lr = 0; lr < 2; lr++) {
            c[lr] = cal_mix_dc(cal_live.mix_off[lr]);
        }
        mo = vld1_s32(c);
    }
    for (unsigned f = 0; f < AUDIO_BLOCK; f++) {
        int32_t t[8] __attribute__((aligned(16)));
        int32x4x2_t v = vld2q_s32(out[f]);      /* [slot][serializer] */
        vst1q_s32(t, vqaddq_s32(v.val[0], co.val[0]));
        vst1q_s32(t + 4, vqaddq_s32(v.val[1], co.val[1]));
        burst8(&m0_buf[half][f][0][0], t);
        vst1_s32(mix_ready[f], cal_mix_words(mo, vld1_s32(mix[f])));
    }
    blocks++;
    prof_to(PS_REST);
    prof_block();
}

static void copy_mix(void)
{
    unsigned m1 = idle_half(EV_MCASP1_TX, 1, m1t_buf[1], sizeof(m1t_buf[1]));

    static int32_t prev[2];

    prof_to(PS_RM1);
    audio_stats.m1_blocks++;
    /* Linear interpolation up to McASP1's rate. */
    for (unsigned f = 0; f < AUDIO_BLOCK; f++) {
        int32_t t[M1_RATIO][2] __attribute__((aligned(16)));
        for (unsigned c = 0; c < 2; c++) {
            int32_t a = prev[c], d = (int32_t)(((int64_t)mix_ready[f][c] - a) / M1_RATIO);
            for (unsigned k = 0; k < M1_RATIO; k++) {
                /* Wraps (a full-scale step overflows d * 4); ends at the target. */
                t[k][c] = (int32_t)((uint32_t)a + (uint32_t)d * (k + 1));
            }
            prev[c] = mix_ready[f][c];
        }
        burst8(&m1t_buf[m1][f * M1_RATIO][0], &t[0][0]);
    }
    prof_to(PS_REST);
}

static uint32_t isr_pending;

static void audio_service(void)
{
    if (isr_pending & (1u << EV_MCASP0_TX)) {
        render_block();
    }
    if (isr_pending & (1u << EV_MCASP1_TX)) {
        copy_mix();
    }
}

/* Nested, so the gate interrupt can timestamp edges during a render. */
static void audio_isr(void)
{
    uint32_t start = time_ticks();
    uint32_t pending = REG(TPCC + SH0_IPR);

    prof_isr_enter();

    DIAG_IRQ(DT_IRQ_ENTRY, pending);

    REG(TPCC + SH0_ICR) = pending & ((1u << EV_MCASP0_TX) | (1u << EV_MCASP1_TX));
    (void)REG(TPCC + SH0_IPR);      /* flush the posted clear (see diag.c stall_tick) */
    isr_pending = pending;
    intc_nest(audio_service);
    uint32_t us = (time_ticks() - start) / 24u;
    if (us > audio_stats.isr_us_max) {
        audio_stats.isr_us_max = us;
    }
    /* The CC interrupts on IPR going from 0 to non-zero; a completion that
     * landed after the IPR read would otherwise never interrupt again. */
    REG(TPCC + SH0_IEVAL) = 1;
    __asm__ volatile("dsb" ::: "memory");
    DIAG_IRQ(DT_IRQ_DONE, 0);
    prof_isr_exit();
}

static void dma_buffers_clear(void)
{
    volatile int32_t *p;

    for (p = &m0_buf[0][0][0][0]; p < &m0_buf[0][0][0][0] + sizeof(m0_buf) / 4; p++) {
        *p = 0;
    }
    for (p = &m1t_buf[0][0][0]; p < &m1t_buf[0][0][0] + sizeof(m1t_buf) / 4; p++) {
        *p = 0;
    }
    for (p = &m1r_buf[0][0][0]; p < &m1r_buf[0][0][0] + sizeof(m1r_buf) / 4; p++) {
        *p = 0;
    }
}

void audio_start(audio_render_fn fn)
{
    render = fn;
    dma_buffers_clear();
    codec_init();
    edma_setup();
    mcasp0_config();
    mcasp1_config();

    /* GBLCTL order as stock: McASP1 high clocks and serializers, then McASP0
     * all the way (it generates the shared bit clock and frame sync), then
     * McASP1's state machines and frame sync. McASP1 never enables its own
     * bit clock dividers: its clocks come in from outside. */
    intc_enable(IRQ_EDMA_COMPLETION, audio_isr);
    edma_enable_events();
    gbl_set(MCASP1, GBL_RHCLKRST);
    gbl_set(MCASP1, GBL_XHCLKRST);
    REG(MCASP1 + RSTAT) = 0xffffu;
    gbl_set(MCASP1, GBL_RSRCLR);
    REG(MCASP1 + XSTAT) = 0xffffu;
    gbl_set(MCASP1, GBL_XSRCLR);
    gbl_set(MCASP0, GBL_XHCLKRST);
    gbl_set(MCASP0, GBL_XCLKRST);
    REG(MCASP0 + XSTAT) = 0xffffu;
    gbl_set(MCASP0, GBL_XSRCLR);
    gbl_set(MCASP0, GBL_XSMRST);
    gbl_set(MCASP0, GBL_XFRST);
    gbl_set(MCASP1, GBL_RSMRST);
    gbl_set(MCASP1, GBL_RFRST);
    gbl_set(MCASP1, GBL_XSMRST);
    gbl_set(MCASP1, GBL_XFRST);
}

static void mcasp1_watchdog(void);

void audio_poll_errors(void)
{
    const uint32_t chans = (1u << EV_MCASP0_TX) | (1u << EV_MCASP1_TX) | (1u << EV_MCASP1_RX);
    uint32_t missed = REG(TPCC + TPCC_EMR) & chans;

    if (missed) {
        audio_stats.missed++;
        REG(TPCC + TPCC_EMCR) = missed;
    }
    if (REG(MCASP0 + XSTAT) & 1u) {
        audio_stats.underruns++;
        REG(MCASP0 + XSTAT) = 1u;
    }
    mcasp1_watchdog();
    audio_stats.m1_xstat = REG(MCASP1 + XSTAT);
    audio_stats.m1_rstat = REG(MCASP1 + RSTAT);
    audio_stats.m1_gblctl = REG(MCASP1 + GBLCTL);
}

static int gbl_try(uint32_t base, uint32_t bits)
{
    REG(base + GBLCTL) |= bits;
    for (uint32_t n = 0; n < 100000u; n++) {
        if ((REG(base + GBLCTL) & bits) == bits) {
            return 1;
        }
    }
    return 0;
}

/* McASP1 (mix out, sample in) restarted while McASP0 keeps running: reset
 * it, re-arm its DMA from the reload sets, then stock's enable order. */
static void mcasp1_restart(void)
{
    const uint32_t ev = (1u << EV_MCASP1_TX) | (1u << EV_MCASP1_RX);
    static const uint32_t steps[] = {
        GBL_RHCLKRST, GBL_XHCLKRST, GBL_RSRCLR, GBL_XSRCLR, GBL_RSMRST, GBL_RFRST,
        GBL_XSMRST, GBL_XFRST
    };

    __asm__ volatile("cpsid i" ::: "memory");
    REG(TPCC + SH0_EECR) = ev;
    REG(MCASP1 + GBLCTL) = 0;
    for (uint32_t n = 0; n < 100000u && REG(MCASP1 + GBLCTL); n++) {
    }
    REG(TPCC + SH0_ECR) = ev;
    REG(TPCC + SH0_SECR) = ev;
    REG(TPCC + TPCC_EMCR) = ev;
    REG(TPCC + SH0_ICR) = 1u << EV_MCASP1_TX;
    param_copy(EV_MCASP1_TX, SET_M1T_A);
    param_copy(EV_MCASP1_RX, SET_M1R_A);
    REG(TPCC + SH0_EESR) = ev;
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (steps[i] == GBL_RSRCLR) {
            REG(MCASP1 + RSTAT) = 0xffffu;
        } else if (steps[i] == GBL_XSRCLR) {
            REG(MCASP1 + XSTAT) = 0xffffu;
        }
        if (!gbl_try(MCASP1, steps[i])) {
            audio_stats.m1_restart_fail++;
            break;
        }
    }
    __asm__ volatile("cpsie i" ::: "memory");
}

/* Hardware run 18: McASP1 stopped (mix counter frozen) while McASP0 ran
 * on, under heavy load. Latch what it looked like, then restart it. */
static void mcasp1_watchdog(void)
{
    static uint32_t m1_seen, m0_mark;

#ifdef TEST_M1_RESTART
    static int forced;
    if (!forced && blocks > 4500 * BLOCKS_PER_32) {
        forced = 1;
        audio_stats.m1_restarts++;
        mcasp1_restart();
        return;
    }
#endif
    if (audio_stats.m1_blocks != m1_seen) {
        m1_seen = audio_stats.m1_blocks;
        m0_mark = blocks;
        return;
    }
    if (blocks < 256 * BLOCKS_PER_32 || blocks - m0_mark < 32 * BLOCKS_PER_32) {
        return;
    }
    audio_stats.m1_stall_xstat = REG(MCASP1 + XSTAT);
    audio_stats.m1_stall_rstat = REG(MCASP1 + RSTAT);
    audio_stats.m1_stall_gblctl = REG(MCASP1 + GBLCTL);
    audio_stats.m1_stall_edma = ((REG(TPCC + SH0_ER) >> 10) & 3u) |
                                (((REG(TPCC + SH0_EER) >> 10) & 3u) << 4) |
                                (((REG(TPCC + SH0_SER) >> 10) & 3u) << 8) |
                                (((REG(TPCC + TPCC_EMR) >> 10) & 3u) << 12);
    audio_stats.m1_stall_ccnt = ((volatile uint32_t *)PARAM(EV_MCASP1_TX))[7];
    audio_stats.m1_restarts++;
    mcasp1_restart();
    m0_mark = blocks;
}

uint32_t audio_blocks(void)
{
    return blocks;
}

static int unmuted;

void audio_mute(void)
{
    REG(GPIO2 + GPIO_CLEAR) = 1u << OUTPUT_UNMUTE_PIN;
    unmuted = 2;
}

int audio_unmute_poll(void)
{

    if (!unmuted && blocks >= UNMUTE_AFTER_BLOCKS) {
        REG(GPIO2 + GPIO_SET) = 1u << OUTPUT_UNMUTE_PIN;
        REG(GPIO2 + GPIO_OE) &= ~(1u << OUTPUT_UNMUTE_PIN);
        unmuted = 1;
    }
    return unmuted;
}
