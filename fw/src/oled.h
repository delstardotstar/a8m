#ifndef OLED_H_INCLUDED
#define OLED_H_INCLUDED

#include <stdint.h>

#define OLED_W 128
#define OLED_H 128

/* RGB565 framebuffer; oled_flush() sends it to the panel. */
extern uint16_t oled_fb[OLED_W * OLED_H];

extern int oled_ready;      /* set once the panel accepts commands */
void oled_init(void);
void oled_flush(void);
/* From fault/watchdog reports: takes the panel over whatever the
 * interrupted code was doing; 0 if there is no panel. */
int oled_panic(void);
/* Master contrast for a level 1..11 (needs oled_init first). */
void oled_set_brightness(int level);
/* Display off/on, RAM and settings kept. */
void oled_sleep(int asleep);
void oled_clear(uint16_t color);
void oled_text(int x, int y, const char *s, uint16_t color, int scale);
/* Text pixels outside [x0, x1) are skipped; oled_clip_x(0, OLED_W) resets. */
void oled_clip_x(int x0, int x1);
/* Text rows outside [y0, y1) are skipped. */
void oled_clip_y(int y0, int y1);

static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

#endif
