/*
 * Host keyboard -> Assimil8or panel. Keys pull the panel's GPIO inputs low
 * (switches and gate inputs are active-low); up/down step the DATA1
 * encoder one detent, Enter clicks it, -/= turn the DATA2 pot, F1-F8 are
 * the gate inputs. Pin roles come from app's button table (see
 * progress.md).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "am335x.h"

typedef struct {
    unsigned key;
    int bank;
    int pin;
} A8Key;

static const A8Key a8_keys[] = {
    /* channel buttons 1-8 */
    { KEY_1, 1, 20 }, { KEY_2, 1, 21 }, { KEY_3, 1, 22 }, { KEY_4, 1, 23 },
    { KEY_5, 1, 24 }, { KEY_6, 1, 25 }, { KEY_7, 1, 26 }, { KEY_8, 1, 27 },
    { KEY_L, 1, 28 },   /* Load */
    { KEY_S, 1, 30 },   /* Save */
    { KEY_U, 1, 31 },   /* Utility */
    { KEY_ENTER, 1, 19 },   /* DATA1 click */
    { KEY_C, 0, 0 },    /* Channels/Select */
    { KEY_Z, 2, 20 },   /* Zones/Select */
    { KEY_A, 0, 1 },    /* Sample */
    { KEY_R, 0, 30 },   /* Sampling Setup */
    { KEY_P, 0, 27 },   /* Pitch */
    { KEY_E, 0, 28 },   /* Level */
    { KEY_M, 0, 31 },   /* Phase Mod */
    { KEY_T, 3, 4 },    /* Mutate */
    { KEY_W, 0, 10 },   /* Pan/Mix */
    { KEY_Q, 2, 9 },    /* Sample Start */
    { KEY_D, 3, 14 },   /* Sample End */
    { KEY_O, 3, 15 },   /* Loop Start */
    { KEY_K, 3, 16 },   /* Loop End/Length */
    { KEY_V, 3, 20 },   /* Envelope */
    { KEY_N, 3, 21 },   /* Loop Mode */
    { KEY_Y, 3, 19 },   /* Play Mode/Latch */
    /* gate inputs 1-8 (read in the audio IRQ at 0x80000d04), active-low */
    { KEY_F1, 0, 14 }, { KEY_F2, 0, 15 }, { KEY_F3, 0, 16 }, { KEY_F4, 0, 12 },
    { KEY_F5, 0, 13 }, { KEY_F6, 0, 22 }, { KEY_F7, 0, 23 }, { KEY_F8, 0, 26 },
};

/*
 * DATA1: GPIO1_16/17. The audio interrupt only latches state changes and the
 * main loop decodes them, so each phase must last several milliseconds.
 */
#define ENC_BANK 1
#define ENC_A 16
#define ENC_B 17
#define A8_ENC_PHASE_NS (20 * SCALE_MS * a8_timescale)

typedef struct {
    DeviceState *gpio[4];
    Object *adc;
    QEMUTimer *timer;
    int steps;
    int phase;
} A8Panel;

static A8Panel a8_panel;

static void a8_panel_set(int bank, int pin, bool pressed)
{
    qemu_set_irq(qdev_get_gpio_in(a8_panel.gpio[bank], pin), !pressed);
}

/* One detent, (A, B) levels, starting and ending with both high. */
static const uint8_t a8_enc_seq[2][4][2] = {
    { { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 1 } },
    { { 1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 } },
};

static void a8_panel_enc_tick(void *opaque)
{
    A8Panel *p = opaque;
    const uint8_t *st = a8_enc_seq[p->steps < 0][p->phase];

    qemu_set_irq(qdev_get_gpio_in(p->gpio[ENC_BANK], ENC_A), st[0]);
    qemu_set_irq(qdev_get_gpio_in(p->gpio[ENC_BANK], ENC_B), st[1]);
    if (++p->phase == 4) {
        p->phase = 0;
        p->steps += p->steps > 0 ? -1 : 1;
        if (!p->steps) {
            return;
        }
    }
    timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + A8_ENC_PHASE_NS);
}

static void a8_panel_event(DeviceState *dev, QemuConsole *src,
                           QemuInputEvent *evt)
{
    unsigned key = evt->key.key;
    bool down = evt->key.down;

    if ((key == KEY_MINUS || key == KEY_EQUAL) && down) {
        /* DATA2 pot on AIN0, 1/32 of travel per press; wired inverted. */
        int v = object_property_get_uint(a8_panel.adc, "ain0", &error_abort);
        v = MIN(MAX(v + (key == KEY_EQUAL ? -128 : 128), 0), 4095);
        object_property_set_uint(a8_panel.adc, "ain0", v, &error_abort);
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        if (down) {
            bool idle = !a8_panel.steps;
            a8_panel.steps += key == KEY_UP ? 1 : -1;
            if (idle && a8_panel.steps) {
                a8_panel.phase = 0;
                a8_panel_enc_tick(&a8_panel);
            }
        }
        return;
    }
    for (size_t i = 0; i < ARRAY_SIZE(a8_keys); i++) {
        if (a8_keys[i].key == key) {
            a8_panel_set(a8_keys[i].bank, a8_keys[i].pin, down);
        }
    }
}

static const QemuInputHandler a8_panel_handler = {
    .name = "assimil8or panel",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = a8_panel_event,
};

void a8_panel_init(DeviceState *gpio[4], Object *adc)
{
    a8_panel.adc = adc;
    QemuInputHandlerState *hs;

    memcpy(a8_panel.gpio, gpio, sizeof(a8_panel.gpio));
    a8_panel.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, a8_panel_enc_tick,
                                  &a8_panel);
    hs = qemu_input_handler_register(gpio[0], &a8_panel_handler);
    qemu_input_handler_activate(hs);
}
