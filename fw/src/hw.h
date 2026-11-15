/* AM335x registers used by the firmware (TRM SPRUH73) and Assimil8or board
 * wiring found by running the stock app in the emulator (progress.md). */
#ifndef HW_H
#define HW_H

#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* Clock modules. */
#define CM_PER          0x44e00000u
#define CM_WKUP         0x44e00400u
#define CM_PER_GPMC     0x030u
#define CM_PER_GPIO1    0x0acu
#define CM_PER_GPIO2    0x0b0u
#define CM_PER_GPIO3    0x0b4u
#define CM_WKUP_GPIO0   0x008u
#define CLK_ENABLE      0x2u
#define CLK_ENABLE_DBCLK 0x00040002u
#define CLK_IDLEST_MASK 0x00030000u

/* Pad control: mode in bits 0-2, pull disable bit 3, pull-up bit 4,
 * receiver enable bit 5. */
#define CONTROL         0x44e10000u
#define PAD_MODE7_OUT   0x4fu
#define PAD_MODE0_IN    0x68u
#define PAD_MODE0_OUT   0x48u

/* GPIO banks. */
#define GPIO0           0x44e07000u
#define GPIO1           0x4804c000u
#define GPIO2           0x481ac000u
#define GPIO3           0x481ae000u
#define GPIO_OE         0x134u
#define GPIO_DATAIN     0x138u
#define GPIO_CLEAR      0x190u
#define GPIO_SET        0x194u

/* GPMC chip select 0. */
#define GPMC            0x50000000u
#define GPMC_SYSCONFIG  0x010u
#define GPMC_CS0_CONFIG(n) (0x060u + 4u * ((n) - 1u))

/* OLED (SSD1351) on GPMC CS0; address bit 18 drives D/C. */
#define OLED_CMD        (*(volatile uint8_t *)0x01000000u)
#define OLED_DATA       (*(volatile uint8_t *)0x01040000u)
#define OLED_RESET_GPIO GPIO2
#define OLED_RESET_PIN  2
#define OLED_POWER_GPIO GPIO2   /* low = panel supply on (stock boot/shutdown) */
#define OLED_POWER_PIN  5

/* Play Mode LEDs (1 SHOT / GATED) and Loop Mode's red LED. */
#define LED_GPIO        GPIO2
#define LED_A_PIN       7       /* GPIO2 */
#define LED_B_PIN       8       /* GPIO2 */
#define LED_LOOP_RED_PIN 9      /* GPIO0 */
#define PAD_LED_A       0x8a4u  /* lcd_data1 */
#define PAD_LED_B       0x8a8u  /* lcd_data2 */
#define PAD_LED_LOOP_RED 0x8dcu /* lcd_data13 */

/* Clock domains (CM_PER *_CLKSTCTRL) and their activity bits. */
#define CM_PER_L4LS_CLKSTCTRL   0x000u
#define CM_PER_L3S_CLKSTCTRL    0x004u
#define CM_PER_L3_CLKSTCTRL     0x00cu
#define CM_PER_OCPWP_L3_CLKSTCTRL 0x12cu
#define CM_PER_L4LS_CLKCTRL     0x060u
#define CM_PER_L3_INSTR_CLKCTRL 0x0dcu
#define CM_PER_L3_CLKCTRL       0x0e0u
#define CLKSTCTRL_SW_WKUP       0x2u
#define CLKSTCTRL_ACTIVITY      0xfffffffcu
#define CLOCK_TIMEOUT           1000000u

/* Module clock on and functional; 0 on timeout. */
static inline int clock_enable(uint32_t module, uint32_t offset, uint32_t value)
{
    REG(module + offset) = value;
    for (uint32_t n = 0; n < CLOCK_TIMEOUT; n++) {
        if (!(REG(module + offset) & CLK_IDLEST_MASK)) {
            return 1;
        }
    }
    return 0;
}

/* Force a clock domain awake; 0 if no clock becomes active. */
static inline int domain_wake(uint32_t module, uint32_t offset)
{
    REG(module + offset) = CLKSTCTRL_SW_WKUP;
    for (uint32_t n = 0; n < CLOCK_TIMEOUT; n++) {
        if (REG(module + offset) & CLKSTCTRL_ACTIVITY) {
            return 1;
        }
    }
    return 0;
}

/* As clock_enable/domain_wake, but a timeout stops in diag_fail(). */
void clock_require(uint32_t module, uint32_t offset, uint32_t value);
void domain_require(uint32_t module, uint32_t offset);
void clocks_init(void);

#endif
