#include "board.h"
#include "hw.h"

void board_pads_init(void)
{
    for (unsigned i = 0; i < board_pad_count; i++) {
        REG(CONTROL + board_pads[i].offset) = board_pads[i].value;
    }
}
