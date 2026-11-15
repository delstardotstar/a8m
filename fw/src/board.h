#ifndef BOARD_H_INCLUDED
#define BOARD_H_INCLUDED

#include <stdint.h>

struct pad_setting {
    uint16_t offset;    /* from CONTROL */
    uint8_t value;
};

extern const struct pad_setting board_pads[];
extern const unsigned board_pad_count;

/* Applies the stock pad table. */
void board_pads_init(void);

#endif
