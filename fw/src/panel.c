#include "panel.h"
#include "hw.h"
#include "diag.h"
#include "systime.h"
#include "intc.h"

/* Board wiring found in the emulator (progress.md "Panel map"). Switches and
 * gate inputs are active-low. */
struct pin {
    uint8_t bank;
    uint8_t bit;
    uint16_t pad;
};

static const struct pin buttons[BTN_COUNT] = {
    [BTN_CH1] = { 1, 20, 0x850 },           [BTN_CH2] = { 1, 21, 0x854 },
    [BTN_CH3] = { 1, 22, 0x858 },           [BTN_CH4] = { 1, 23, 0x85c },
    [BTN_CH5] = { 1, 24, 0x860 },           [BTN_CH6] = { 1, 25, 0x864 },
    [BTN_CH7] = { 1, 26, 0x868 },           [BTN_CH8] = { 1, 27, 0x86c },
    [BTN_LOAD] = { 1, 28, 0x878 },          [BTN_SAVE] = { 1, 30, 0x880 },
    [BTN_UTILITY] = { 1, 31, 0x884 },       [BTN_CHANNELS] = { 0, 0, 0x948 },
    [BTN_SAMPLE] = { 0, 1, 0x94c },         [BTN_SAMPLING_SETUP] = { 0, 30, 0x870 },
    [BTN_ZONES] = { 2, 20, 0x93c },         [BTN_PITCH] = { 0, 27, 0x82c },
    [BTN_LEVEL] = { 0, 28, 0x928 },         [BTN_PHASE_MOD] = { 0, 31, 0x874 },
    [BTN_MUTATE] = { 3, 4, 0x918 },         [BTN_PAN_MIX] = { 0, 10, 0x8d8 },
    [BTN_SAMPLE_START] = { 2, 9, 0x8ac },   [BTN_SAMPLE_END] = { 3, 14, 0x990 },
    [BTN_LOOP_START] = { 3, 15, 0x994 },    [BTN_LOOP_END] = { 3, 16, 0x998 },
    [BTN_ENVELOPE] = { 3, 20, 0x9a8 },      [BTN_LOOP_MODE] = { 3, 21, 0x9ac },
    [BTN_PLAY_MODE] = { 3, 19, 0x9a4 },     [BTN_DATA1] = { 1, 19, 0x84c },
};

static const char *const names[BTN_COUNT] = {
    "CH1", "CH2", "CH3", "CH4", "CH5", "CH6", "CH7", "CH8",
    "LOAD", "SAVE", "UTILITY", "CHANNELS", "SAMPLE", "SAMPLING SETUP",
    "ZONES", "PITCH", "LEVEL", "PHASE MOD", "MUTATE", "PAN/MIX",
    "SAMPLE START", "SAMPLE END", "LOOP START", "LOOP END", "ENVELOPE",
    "LOOP MODE", "PLAY MODE", "DATA1 CLICK",
};

static const struct pin gates[8] = {
    { 0, 14, 0x980 }, { 0, 15, 0x984 }, { 0, 16, 0x91c }, { 0, 12, 0x978 },
    { 0, 13, 0x97c }, { 0, 22, 0x820 }, { 0, 23, 0x824 }, { 0, 26, 0x828 },
};

static const struct pin encoder_a = { 1, 16, 0x840 };
static const struct pin encoder_b = { 1, 17, 0x844 };

#define PAD_GPIO_IN_PULLUP 0x77u
#define PAD_GPIO_IN        0x6fu
#define DEBOUNCE_TICKS     (5000u * 24u)
#define MAX_CHANGES        3

static const uint32_t bank_base[4] = { GPIO0, GPIO1, GPIO2, GPIO3 };

#define GPIO_IRQSTATUS_RAW0 0x024u
#define GPIO_IRQSTATUS0     0x02cu
#define GPIO_IRQSTATUS_SET0 0x034u
#define GPIO_IRQSTATUS_CLR0 0x03cu
#define GPIO_RISINGDETECT   0x148u
#define GPIO_FALLINGDETECT  0x14cu

static uint32_t gate_mask;

#define EDGE_RING 64
static struct gate_edge edge_ring[EDGE_RING];
static volatile uint32_t edge_wr, edge_rd;
static uint8_t edge_level;
static int edge_irq_on;
static uint32_t edge_stuck;

/* TSC_ADC: one step on AIN0 (DATA2), continuous, results to FIFO0. */
#define ADC_TSC            0x44e0d000u
#define ADC_CTRL           0x040u
#define ADC_CLKDIV         0x04cu
#define ADC_STEPENABLE     0x054u
#define ADC_STEPCONFIG1    0x064u
#define ADC_FIFO0COUNT     0x0e4u
#define ADC_FIFO0DATA      0x100u
#define ADC_CTRL_ENABLE    0x1u
#define ADC_CTRL_STEP_WRITABLE 0x4u
#define ADC_STEP_DATA2     0x01843011u /* continuous, 16x average, AIN0 vs VREFP/VREFN */
#define CM_WKUP_ADC_TSC    0x0bcu

/* Buttons are sampled with the encoder (audio interrupt and panel_poll):
 * a tap shorter than a main-loop pass (an OLED flush) still counts. */
static uint32_t raw_held, stable_held, change_at, latch_pressed, latch_released;
static int rejecting;
uint32_t panel_rejects;
static volatile uint8_t enc_state;
static volatile int enc_accum, enc_detents;
static uint16_t data2;

/* ADC jitter would wiggle absolute DATA2 values: smooth, then hold still
 * until the reading leaves a +-D2_HYST band, rescaled to keep 0..4095. */
#define D2_HYST 10
#define D2_END  40
static int32_t d2_avg = -1, d2_held;

static void data2_in(uint32_t raw)
{
    int32_t x = (int32_t)raw << 4;

    d2_avg = d2_avg < 0 ? x : d2_avg + (x - d2_avg) / 4;
    x = d2_avg >> 4;
    if (x > d2_held + D2_HYST) {
        d2_held = x - D2_HYST;
    } else if (x < d2_held - D2_HYST) {
        d2_held = x + D2_HYST;
    }
    /* The pot doesn't quite reach 0 / 4095 on hardware: small end zones. */
    x = d2_held - D2_HYST - D2_END;
    x = x < 0 ? 0 : x * 4095 / (4095 - 2 * (D2_HYST + D2_END));
    data2 = (uint16_t)(x > 4095 ? 4095 : x);
}

static int read_pin(const struct pin *p)
{
    return (REG(bank_base[p->bank] + GPIO_DATAIN) >> p->bit) & 1u;
}

static void input_pad(const struct pin *p, uint32_t value)
{
    REG(CONTROL + p->pad) = value;
    REG(bank_base[p->bank] + GPIO_OE) |= 1u << p->bit;
}

static uint8_t gate_bits(uint32_t pins)
{
    uint8_t g = 0;

    for (int i = 0; i < 8; i++) {
        g |= (uint8_t)(((pins >> gates[i].bit) & 1u) << i);
    }
    return g;
}

static volatile uint8_t edge_lost;

/* Edges were dropped (ring full): the reader takes the pins' level again. */
int panel_gate_edge_lost(void)
{
    int l = edge_lost;

    edge_lost = 0;
    return l;
}

static void edge_push(uint32_t t, int ch, int on)
{
    uint32_t w = edge_wr;

    if (w - edge_rd >= EDGE_RING) {
        edge_lost = 1;
        return;
    }
    edge_ring[w % EDGE_RING] = (struct gate_edge){ t, (uint8_t)ch, (uint8_t)on };
    __asm__ volatile("dmb" ::: "memory");
    edge_wr = w + 1;
}

static void gate_isr(void)
{
    uint32_t t = time_ticks();
    uint32_t st = REG(GPIO0 + GPIO_IRQSTATUS0) & gate_mask;

    REG(GPIO0 + GPIO_IRQSTATUS0) = st;
    (void)REG(GPIO0 + GPIO_IRQSTATUS0);     /* posted write */
    uint8_t now = panel_gates_now(), changed = gate_bits(st);
    for (int i = 0; i < 8; i++) {
        if ((changed >> i) & 1) {
            int on = (now >> i) & 1;
            if (on == ((edge_level >> i) & 1)) {
                edge_push(t, i, !on);       /* both edges since the last interrupt */
            }
            edge_push(t, i, on);
        }
    }
    edge_level = (uint8_t)((edge_level & ~changed) | (now & changed));
}

const struct gate_edge *panel_gate_edge_peek(void)
{
    uint32_t r = edge_rd;

    if (r == edge_wr) {
        return 0;
    }
    __asm__ volatile("dmb" ::: "memory");
    return &edge_ring[r % EDGE_RING];
}

void panel_gate_edge_pop(void)
{
    __asm__ volatile("dmb" ::: "memory");
    edge_rd++;
}

int panel_gate_irq_on(void)
{
    return edge_irq_on;
}

/* An edge still latched one poll later means the interrupt never came. */
int panel_gate_irq(void)
{
    if (!edge_irq_on) {
        return 0;
    }
    uint32_t raw = REG(GPIO0 + GPIO_IRQSTATUS_RAW0) & gate_mask;
    if (raw & edge_stuck) {
        REG(GPIO0 + GPIO_IRQSTATUS_CLR0) = gate_mask;
        REG(GPIO0 + GPIO_RISINGDETECT) &= ~gate_mask;
        edge_irq_on = 0;
        return 0;
    }
    edge_stuck = raw;
    return 1;
}

void panel_init(void)
{
    clock_require(CM_WKUP, CM_WKUP_GPIO0, CLK_ENABLE_DBCLK);
    clock_require(CM_PER, CM_PER_GPIO1, CLK_ENABLE_DBCLK);
    clock_require(CM_PER, CM_PER_GPIO2, CLK_ENABLE_DBCLK);
    clock_require(CM_PER, CM_PER_GPIO3, CLK_ENABLE_DBCLK);
    for (int i = 0; i < BTN_COUNT; i++) {
        input_pad(&buttons[i], PAD_GPIO_IN_PULLUP);
    }
    input_pad(&encoder_a, PAD_GPIO_IN_PULLUP);
    input_pad(&encoder_b, PAD_GPIO_IN_PULLUP);
    for (int i = 0; i < 8; i++) {
        input_pad(&gates[i], PAD_GPIO_IN);
        gate_mask |= 1u << gates[i].bit;
    }
    /* Both edges interrupt and are timestamped (gate_isr). Polling reads
     * rises (falling pin) latched in the raw status instead, so pulses
     * shorter than a block still count. */
    REG(GPIO0 + GPIO_IRQSTATUS0) = gate_mask;
    REG(GPIO0 + GPIO_FALLINGDETECT) |= gate_mask;
#ifndef GATE_POLL
    edge_level = panel_gates_now();
    REG(GPIO0 + GPIO_RISINGDETECT) |= gate_mask;
    edge_irq_on = 1;
    intc_enable_urgent(IRQ_GPIO0A, gate_isr);
    REG(GPIO0 + GPIO_IRQSTATUS_SET0) = gate_mask;
#endif
    enc_state = (uint8_t)((read_pin(&encoder_a) << 1) | read_pin(&encoder_b));

    clock_require(CM_WKUP, CM_WKUP_ADC_TSC, CLK_ENABLE);
    REG(ADC_TSC + ADC_CTRL) = ADC_CTRL_STEP_WRITABLE;
    REG(ADC_TSC + ADC_CLKDIV) = 7;
    REG(ADC_TSC + ADC_STEPCONFIG1) = ADC_STEP_DATA2;
    REG(ADC_TSC + ADC_STEPENABLE) = 1u << 1;
    REG(ADC_TSC + ADC_CTRL) = ADC_CTRL_STEP_WRITABLE | ADC_CTRL_ENABLE;
}

/* Quadrature: +1/-1 per valid transition, a detent on reaching the rest
 * state (both high). The sign is as stock reads the same pins: + moves
 * down a list. */
static void encoder_sample(void)
{
    static const int8_t delta[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
    uint8_t now = (uint8_t)((read_pin(&encoder_a) << 1) | read_pin(&encoder_b));

    if (now == enc_state) {
        return;
    }
    enc_accum += delta[(enc_state << 2) | now];
    enc_state = now;
    if (now == 3 && enc_accum) {
        enc_detents -= enc_accum / 4 ? enc_accum / 4 : (enc_accum > 0 ? 1 : -1);
        enc_accum = 0;
    }
}

static void buttons_sample(void)
{
    uint32_t in[4], raw = 0, now = time_ticks();

    for (int b = 0; b < 4; b++) {
        in[b] = ~REG(bank_base[b] + GPIO_DATAIN);
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        raw |= ((in[buttons[i].bank] >> buttons[i].bit) & 1u) << i;
    }
    if (raw != raw_held) {
        raw_held = raw;
        change_at = now;
    }
    if (raw_held == stable_held) {
        rejecting = 0;
    } else if (now - change_at >= DEBOUNCE_TICKS) {
        if (__builtin_popcount(raw_held ^ stable_held) > MAX_CHANGES) {
            panel_rejects += !rejecting;
            rejecting = 1;
        } else {
            latch_pressed |= raw_held & ~stable_held;
            latch_released |= stable_held & ~raw_held;
            stable_held = raw_held;
            rejecting = 0;
        }
    }
}

/* Sampled from the audio interrupt, like stock's encoder, so phases and
 * taps are not missed while the main loop is busy, and from panel_poll
 * with interrupts masked. */
void panel_sample(void)
{
    encoder_sample();
    buttons_sample();
}

void panel_poll(struct panel *p)
{
    __asm__ volatile("cpsid i" ::: "memory");
    panel_sample();
    p->pressed = latch_pressed;
    p->released = latch_released;
    p->held = stable_held;
    latch_pressed = latch_released = 0;
    p->data1_steps = enc_detents;
    enc_detents = 0;
    __asm__ volatile("cpsie i" ::: "memory");

    {
        uint32_t sum = 0, n = 0;

        while (REG(ADC_TSC + ADC_FIFO0COUNT) & 0x7fu) {
            /* The pot is wired so clockwise lowers the reading. */
            sum += 4095u - (REG(ADC_TSC + ADC_FIFO0DATA) & 0xfffu);
            n++;
        }
        if (n) {
            data2_in(sum / n);
        }
    }
    p->data2 = data2;

    p->gates = 0;
    for (int i = 0; i < 8; i++) {
        p->gates |= (uint8_t)(!read_pin(&gates[i]) << i);
    }
}

uint8_t panel_gate_edges(void)
{
    uint32_t raw = REG(GPIO0 + GPIO_IRQSTATUS_RAW0) & gate_mask;

    if (raw) {
        REG(GPIO0 + GPIO_IRQSTATUS0) = raw;
    }
    return gate_bits(raw);
}

uint8_t panel_gates_now(void)
{
    return gate_bits(~REG(GPIO0 + GPIO_DATAIN));
}

const char *panel_button_name(enum button b)
{
    return b < BTN_COUNT ? names[b] : "?";
}
