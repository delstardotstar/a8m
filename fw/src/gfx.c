#include "gfx.h"
#include "font.h"

static inline uint16_t px(uint16_t c)
{
    return __builtin_bswap16(c);
}

void gfx_clear(void)
{
    oled_clear(0);
}

static int clip_y0, clip_y1 = OLED_H;

void gfx_clip_y(int y0, int y1)
{
    clip_y0 = y0;
    clip_y1 = y1;
    oled_clip_y(y0, y1);
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    uint16_t p = px(c);

    for (int j = y < clip_y0 ? clip_y0 : y; j < y + h && j < clip_y1; j++) {
        for (int i = x < 0 ? 0 : x; i < x + w && i < OLED_W; i++) {
            oled_fb[j * OLED_W + i] = p;
        }
    }
}

void gfx_frame(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

int gfx_text_width(const char *s)
{
    int n = 0;

    while (*s++) {
        n++;
    }
    return n * GFX_CHAR_W;
}

int gfx_text(int x, int y, const char *s, uint16_t c)
{
    oled_text(x, y, s, c, 1);
    return x + gfx_text_width(s);
}

void gfx_text_right(int x_right, int y, const char *s, uint16_t c)
{
    oled_text(x_right - gfx_text_width(s) + 1, y, s, c, 1);
}

int gfx_title(int x, int y, const char *s, uint16_t c)
{
    char one[2] = { 0, 0 };

    for (; *s; s++, x += GFX_CHAR_W + 1) {
        one[0] = *s;
        oled_text(x, y, one, c, 1);
        oled_text(x + 1, y, one, c, 1);
    }
    return x;
}

void gfx_text_box(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    gfx_rect(x - 1, y - 1, gfx_text_width(s) + 1, FONT_H + 2, bg);
    oled_text(x, y, s, fg, 1);
}

static float clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

void gfx_bar(int x, int y, int w, int h, float fill, uint16_t c)
{
    gfx_frame(x, y, w, h, C_DIM);
    gfx_rect(x + 1, y + 1, (int)((float)(w - 2) * clamp01(fill) + 0.5f), h - 2, c);
}

void gfx_bar_bipolar(int x, int y, int w, int h, float v, uint16_t c)
{
    int mid = x + w / 2, len;

    gfx_frame(x, y, w, h, C_DIM);
    v = v < -1 ? -1 : v > 1 ? 1 : v;
    len = (int)((float)(w / 2 - 1) * (v < 0 ? -v : v) + 0.5f);
    if (v < 0) {
        gfx_rect(mid - len, y + 1, len, h - 2, c);
    } else {
        gfx_rect(mid, y + 1, len, h - 2, c);
    }
    gfx_rect(mid, y, 1, h, C_WHITE);
}
