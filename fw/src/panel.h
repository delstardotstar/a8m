#ifndef PANEL_H_INCLUDED
#define PANEL_H_INCLUDED

#include <stdint.h>

/* Front panel buttons, in the stock firmware's button-table order. */
enum button {
    BTN_CH1, BTN_CH2, BTN_CH3, BTN_CH4, BTN_CH5, BTN_CH6, BTN_CH7, BTN_CH8,
    BTN_LOAD, BTN_SAVE, BTN_UTILITY, BTN_CHANNELS, BTN_SAMPLE, BTN_SAMPLING_SETUP,
    BTN_ZONES, BTN_PITCH, BTN_LEVEL, BTN_PHASE_MOD, BTN_MUTATE, BTN_PAN_MIX,
    BTN_SAMPLE_START, BTN_SAMPLE_END, BTN_LOOP_START, BTN_LOOP_END, BTN_ENVELOPE,
    BTN_LOOP_MODE, BTN_PLAY_MODE, BTN_DATA1,
    BTN_COUNT
};

struct panel {
    uint32_t held;          /* bit per enum button, debounced */
    uint32_t pressed;       /* went down since the last poll */
    uint32_t released;      /* went up since the last poll */
    int data1_steps;        /* encoder detents since the last poll, + clockwise */
    uint16_t data2;         /* pot, 0..4095 */
    uint8_t gates;          /* bit per channel, high = gate on */
};

void panel_init(void);
void panel_poll(struct panel *p);
/* Debounced button frames ignored for changing too many buttons at once
 * (a misread bank, not a hand). */
extern uint32_t panel_rejects;
const char *panel_button_name(enum button b);
/* Samples the DATA1 encoder and the buttons; call often (the audio
 * interrupt does). */
void panel_sample(void);
/* Gates that rose since the last call, bit per channel (edge latched in
 * hardware); safe from interrupts. */
uint8_t panel_gate_edges(void);
/* Gate inputs now, bit per channel (all on GPIO0; safe from interrupts). */
uint8_t panel_gates_now(void);
/* Gate edges timestamped (time_ticks) by the GPIO interrupt, oldest first;
 * peek returns 0 when there is none. Audio interrupt only. */
struct gate_edge {
    uint32_t t;
    uint8_t ch, on;
};
const struct gate_edge *panel_gate_edge_peek(void);
void panel_gate_edge_pop(void);
int panel_gate_edge_lost(void);
/* 1 while edges come from the interrupt, else poll panel_gate_edges()
 * (from then on, if the interrupt never fired). Once per block. */
int panel_gate_irq(void);
int panel_gate_irq_on(void);

#endif
