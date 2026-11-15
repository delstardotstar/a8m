#ifndef LEDS_H_INCLUDED
#define LEDS_H_INCLUDED

#include <stdint.h>

/* Channel LEDs: channels 1-7 on GPIO2_11..17, channel 8 on GPIO0_8, active
 * high (found by watching stock in the emulator). As stock: a channel's LED
 * stays lit while whatever fired it (its own or its Master's gate, or a
 * button) is held, and at least FLASH_US after each trigger so short
 * triggers show. A latched channel's LED flashes. held = button bits
 * (panel.h). Main loop only. */
void leds_init(void);
void leds_update(uint32_t held);
/* PLAY MODE LEDs: play 1 = 1 SHOT, 0 = GATED, -1 = off; LOOP MODE LEDs:
 * loop 0 off, 1 Normal (top), 2 Gated (bottom). Main loop, after boot
 * (the boot stages use them first). */
void leds_modes(int play, int loop);

#endif
