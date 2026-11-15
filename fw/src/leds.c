#include "leds.h"
#include "engine.h"
#include "hw.h"
#include "panel.h"
#include "rec.h"
#include "systime.h"

#define FLASH_US 50000u
#define LATCH_BLINK_US 125000u

static uint32_t mask2 = 0x7fu << 11, mask0 = 1u << 8;
/* Mode LEDs (stock, in the emulator): GPIO2 7 1 SHOT, 8 GATED; GPIO0 9
 * loop Normal (top), 11 loop Gated (bottom). */
#define PLAY_LEDS2  ((1u << 7) | (1u << 8))
#define LOOP_LEDS0  ((1u << 9) | (1u << 11))
/* SAMPLE (stock, in the emulator): GPIO2 6, flashing while armed. */
#define SAMPLE_LED2 (1u << 6)
#define ARMED_BLINK_US 300000u
static uint32_t seen[CHANNELS];
static uint64_t until[CHANNELS];
static uint8_t holding[CHANNELS];   /* source still held since this channel fired */

void leds_init(void)
{
    REG(GPIO2 + GPIO_CLEAR) = mask2;
    REG(GPIO0 + GPIO_CLEAR) = mask0;
    REG(GPIO2 + GPIO_CLEAR) = SAMPLE_LED2;
    REG(GPIO2 + GPIO_OE) &= ~(mask2 | PLAY_LEDS2 | SAMPLE_LED2);
    REG(GPIO0 + GPIO_OE) &= ~(mask0 | LOOP_LEDS0);
}

void leds_modes(int play, int loop)
{
    uint32_t on2 = play < 0 ? 0 : play ? 1u << 7 : 1u << 8;
    uint32_t on0 = loop == 1 ? 1u << 9 : loop == 2 ? 1u << 11 : 0;

    REG(GPIO2 + GPIO_SET) = on2;
    REG(GPIO2 + GPIO_CLEAR) = PLAY_LEDS2 & ~on2;
    REG(GPIO0 + GPIO_SET) = on0;
    REG(GPIO0 + GPIO_CLEAR) = LOOP_LEDS0 & ~on0;
}

void leds_update(uint32_t held)
{
    uint64_t now = time_us64();
    uint32_t on2 = 0, on0 = 0;
    uint8_t gates = engine_status.gates, latched = engine_status.latched;
    int blink = (now / LATCH_BLINK_US) & 1u;

    for (int ch = 0; ch < CHANNELS; ch++) {
        uint32_t n = engine_status.triggers[ch];
        int src = engine_status.trigger_src[ch];
        int still_held;

        if (n != seen[ch]) {
            seen[ch] = n;
            until[ch] = now + FLASH_US;
            holding[ch] = 1;
        }
        still_held = src < 8 ? (gates >> src) & 1 :
                     src < TRIG_SRC_LATCH ? (held >> (BTN_CH1 + src - 8)) & 1 :
                     src < TRIG_SRC_AUTO ? (latched >> (src - TRIG_SRC_LATCH)) & 1 : 0;
        if (!still_held && until[ch] <= now) {
            holding[ch] = 0;
        }
        if ((latched >> ch) & 1 ? blink :
            until[ch] > now || (holding[ch] && still_held)) {
            if (ch < 7) {
                on2 |= 1u << (11 + ch);
            } else {
                on0 |= mask0;
            }
        }
    }
    if (rec_state() == REC_RECORDING || (rec_armed() && (now / ARMED_BLINK_US) & 1u)) {
        on2 |= SAMPLE_LED2;
    }
    REG(GPIO2 + GPIO_SET) = on2;
    REG(GPIO2 + GPIO_CLEAR) = (mask2 | SAMPLE_LED2) & ~on2;
    REG(GPIO0 + GPIO_SET) = on0;
    REG(GPIO0 + GPIO_CLEAR) = mask0 & ~on0;
}
