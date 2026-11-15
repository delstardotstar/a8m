#ifndef GFX_H_INCLUDED
#define GFX_H_INCLUDED

#include <stdint.h>
#include "oled.h"

/* Palette, plain RGB565 (gfx converts to the panel's byte order). */
#define C_BLACK     0x0000u
#define C_WHITE     0xffffu
#define C_TITLE     0x055cu     /* A8 cyan-blue */
#define C_DIM       0x6b4du
#define C_FAINT     0x31a6u
#define C_RED       0xe144u
#define C_DRED      0x7082u
#define C_GREEN     0x3ee7u
#define C_YELLOW    0xfea0u
#define C_ORANGE    0xfbc0u     /* Elektron-ish accent: fine edit, value bars */
#define C_SELBAR    0x036du     /* list highlight */

#define GFX_CHAR_W  6
#define GFX_LINE_H  9

void gfx_clear(void);
/* Drawing outside rows [y0, y1) is skipped; gfx_clip_y(0, OLED_H) resets. */
void gfx_clip_y(int y0, int y1);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_frame(int x, int y, int w, int h, uint16_t c);
/* Returns the x after the text. */
int gfx_text(int x, int y, const char *s, uint16_t c);
void gfx_text_right(int x_right, int y, const char *s, uint16_t c);
/* Bold title (each glyph drawn twice, 7 px advance). */
int gfx_title(int x, int y, const char *s, uint16_t c);
/* Text on a filled box (selection). */
void gfx_text_box(int x, int y, const char *s, uint16_t fg, uint16_t bg);
int gfx_text_width(const char *s);
/* Horizontal bar: outline, fill 0..1 from the left. */
void gfx_bar(int x, int y, int w, int h, float fill, uint16_t c);
/* Bipolar bar: fill -1..1 from the centre. */
void gfx_bar_bipolar(int x, int y, int w, int h, float v, uint16_t c);

#endif
