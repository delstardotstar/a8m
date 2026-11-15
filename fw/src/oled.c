#include "oled.h"
#include "hw.h"
#include "diag.h"
#include "font.h"

/* Panel byte order (high byte first), RGB565 with red in the top bits:
 * with the stock remap (A0h = 66h) the module shows the top bits as red
 * (hardware run 4: blue-in-top text showed red/blue swapped). */
uint16_t oled_fb[OLED_W * OLED_H];
int oled_ready;

static void delay(volatile uint32_t n)
{
    while (n--) {
    }
}

static void cmd(uint8_t c, int nargs, ...);

static void cmd(uint8_t c, int nargs, ...)
{
    __builtin_va_list ap;

    OLED_CMD = c;
    __builtin_va_start(ap, nargs);
    while (nargs--) {
        OLED_DATA = (uint8_t)__builtin_va_arg(ap, int);
    }
    __builtin_va_end(ap);
}

static void gpmc_cs0(void);

static void gpmc_init(void)
{
    static const uint32_t in_pads[] = { 0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c };

    clock_require(CM_PER, CM_PER_GPMC, CLK_ENABLE);
    for (unsigned i = 0; i < sizeof(in_pads) / sizeof(in_pads[0]); i++) {
        REG(CONTROL + in_pads[i]) = PAD_MODE0_IN;     /* gpmc_ad0-7 */
    }
    REG(CONTROL + 0x87c) = PAD_MODE0_OUT;             /* gpmc_csn0 */
    REG(CONTROL + 0x894) = PAD_MODE0_OUT;             /* gpmc_oen_ren */
    REG(CONTROL + 0x898) = PAD_MODE0_OUT;             /* gpmc_wen */

    REG(GPMC + GPMC_CS0_CONFIG(7)) = 0;
    gpmc_cs0();
}

static void gpmc_cs0(void)
{
    REG(GPMC + GPMC_CS0_CONFIG(1)) = 0;               /* 8-bit, non-multiplexed, async */
    REG(GPMC + GPMC_CS0_CONFIG(2)) = 0x001d1d01;
    REG(GPMC + GPMC_CS0_CONFIG(4)) = 0x15051703;
    REG(GPMC + GPMC_CS0_CONFIG(5)) = 0x00171f1f;
    REG(GPMC + GPMC_CS0_CONFIG(7)) = 0x00000f41;      /* 16 MB at 0x01000000, valid */
}

static void gpio_out(uint32_t bank, int pin, int level)
{
    REG(bank + (level ? GPIO_SET : GPIO_CLEAR)) = 1u << pin;
    REG(bank + GPIO_OE) &= ~(1u << pin);
}

void oled_init(void)
{
    clock_require(CM_PER, CM_PER_GPIO2, CLK_ENABLE_DBCLK);
    REG(CONTROL + 0x890) = PAD_MODE7_OUT;             /* gpio2_2: reset */
    REG(CONTROL + 0x89c) = PAD_MODE7_OUT;             /* gpio2_5: supply */
    gpio_out(OLED_POWER_GPIO, OLED_POWER_PIN, 1);
    diag_stage(STAGE_GPMC_CLOCK);
    gpmc_init();
    diag_stage(STAGE_OLED_RESET);

    gpio_out(OLED_RESET_GPIO, OLED_RESET_PIN, 0);
    delay(200000);
    gpio_out(OLED_RESET_GPIO, OLED_RESET_PIN, 1);
    delay(100000);

    diag_stage(STAGE_OLED_COMMANDS);
    cmd(0xfd, 1, 0x12);             /* unlock */
    cmd(0xfd, 1, 0xb1);             /* allow A2/B1/B3/BB/BE/C1 */
    cmd(0xae, 0);                   /* display off */
    cmd(0xa0, 1, 0x66);             /* 65k colour, COM split, C-B-A, column remap */
    cmd(0xa1, 1, 0x00);             /* start line */
    cmd(0xa2, 1, 0x00);             /* display offset */
    cmd(0xa6, 0);                   /* normal display */
    cmd(0xab, 1, 0x01);             /* internal VDD regulator */
    cmd(0xb1, 1, 0x32);             /* phase lengths */
    cmd(0xb3, 1, 0xf1);             /* clock divider / oscillator */
    cmd(0xb4, 3, 0xa0, 0xb5, 0x55); /* external VSL */
    cmd(0xb6, 1, 0x01);             /* second precharge */
    cmd(0xb9, 0);                   /* default linear grayscale table */
    cmd(0xbb, 1, 0x17);             /* precharge voltage */
    cmd(0xbe, 1, 0x05);             /* VCOMH */
    cmd(0xc1, 3, 0x88, 0x70, 0x88); /* contrast A, B, C */
    cmd(0xc7, 1, 0x0f);             /* master contrast */
    cmd(0xca, 1, 0x7f);             /* mux ratio 128 */

    oled_clear(0);
    oled_flush();
    gpio_out(OLED_POWER_GPIO, OLED_POWER_PIN, 0);
    delay(100000);
    cmd(0xaf, 0);                   /* display on */
    oled_ready = 1;
}

void oled_set_brightness(int level)
{
    /* Master contrast steps (of 15) for levels 1..11; 11 is how the panel
     * was initialised before levels existed. */
    static const uint8_t master[11] = { 0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15 };

    level = level < 1 ? 1 : level > 11 ? 11 : level;
    if (oled_ready) {
        cmd(0xc7, 1, master[level - 1]);
    }
}

void oled_sleep(int asleep)
{
    cmd(asleep ? 0xae : 0xaf, 0);
}

void oled_flush(void)
{
    const uint8_t *p = (const uint8_t *)oled_fb;
    uint32_t tag = diag_main_tag;

    DIAG_MAIN(DT_OLED_FLUSH, 0);
    cmd(0x15, 2, 0, OLED_W - 1);
    cmd(0x75, 2, 0, OLED_H - 1);
    OLED_CMD = 0x5c;
    for (unsigned i = 0; i < sizeof(oled_fb); i++) {
        OLED_DATA = p[i];
    }
    diag_main_tag = tag;
}

/* The transfer path is CPU writes over GPMC (no DMA, no busy state), so an
 * interrupted flush only leaves the panel mid-window; the next command
 * starts over. This makes sure the clock, chip select and panel are on. */
int oled_panic(void)
{
    if (!oled_ready || !clock_enable(CM_PER, CM_PER_GPMC, CLK_ENABLE)) {
        return 0;
    }
    gpmc_cs0();
    cmd(0xfd, 1, 0x12);
    cmd(0xfd, 1, 0xb1);
    cmd(0xc7, 1, 0x0f);
    cmd(0xaf, 0);
    return 1;
}

static inline uint16_t panel_pixel(uint16_t rgb)
{
    return __builtin_bswap16(rgb);
}

void oled_clear(uint16_t color)
{
    uint16_t px = panel_pixel(color);

    for (unsigned i = 0; i < OLED_W * OLED_H; i++) {
        oled_fb[i] = px;
    }
}

static int clip_x0, clip_x1 = OLED_W, clip_y0, clip_y1 = OLED_H;

void oled_clip_y(int y0, int y1)
{
    clip_y0 = y0;
    clip_y1 = y1;
}

void oled_clip_x(int x0, int x1)
{
    clip_x0 = x0 < 0 ? 0 : x0;
    clip_x1 = x1 > OLED_W ? OLED_W : x1;
}

void oled_text(int x, int y, const char *s, uint16_t color, int scale)
{
    uint16_t px = panel_pixel(color);

    for (; *s; s++, x += (FONT_W + 1) * scale) {
        const uint8_t *g = font_glyph(*s);
        for (int col = 0; col < FONT_W; col++) {
            for (int row = 0; row < FONT_H; row++) {
                if (!(g[col] & (1u << row))) {
                    continue;
                }
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px_x = x + col * scale + dx, px_y = y + row * scale + dy;
                        if (px_x >= clip_x0 && px_x < clip_x1 && px_y >= clip_y0 && px_y < clip_y1) {
                            oled_fb[px_y * OLED_W + px_x] = px;
                        }
                    }
                }
            }
        }
    }
}
