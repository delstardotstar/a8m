#ifndef FONT_H_INCLUDED
#define FONT_H_INCLUDED

#include <stdint.h>

#define FONT_W 5
#define FONT_H 7

#define FONT_SUB_L "\001"
#define FONT_SUB_R "\002"

const uint8_t *font_glyph(char c);

#endif
