#ifndef A8_AM335X_H
#define A8_AM335X_H

#define TYPE_A8_INTC "a8-intc"
#define TYPE_A8_SPI "a8-spi"
#define TYPE_A8_I2C "a8-i2c"
#define TYPE_A8_EDMA "a8-edma"
#define TYPE_A8_MMC "a8-mmc"
#define TYPE_A8_TIMER "a8-timer"
#define TYPE_A8_GPIO "a8-gpio"
#define TYPE_A8_MCASP "a8-mcasp"
#define TYPE_SSD1351 "ssd1351"
#define TYPE_A8_TSCADC "a8-tscadc"
#define TYPE_A8_WDT "a8-wdt"

/* QEMU exit status for a WDT1 reset (emu/run.sh boots again). */
#define A8_EXIT_WDT 6

typedef struct DeviceState DeviceState;

/*
 * Time dilation: device clocks run this many times slower than QEMU's
 * virtual clock, giving the guest that much more CPU per emulated second.
 * QEMU executes app's DSP code about 3x slower than the real Cortex-A8.
 * Set from $A8_TIMESCALE by the board.
 */
extern double a8_timescale;
void a8_edma_event(DeviceState *edma, int ev);
void a8_edma_event_now(DeviceState *edma, int ev);
/* Appends the CV inputs' current codes to cv.raw (one McASP0 frame). */
void a8_spi_trace(void);
/* Appends gpio0's input levels to gpio0.raw (one McASP0 frame). */
void a8_gpio_trace(void);
void a8_panel_init(DeviceState *gpio[4], Object *adc);
/* Channel output n's loopback cable into CV nA: false if unpatched. */
bool a8_mcasp_loop_cv(int n, double *volts);
void a8_mcasp_set_clock_slave(DeviceState *master, DeviceState *slave,
                              int frames_per_master);

#endif
