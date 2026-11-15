#ifndef SYSTIME_H_INCLUDED
#define SYSTIME_H_INCLUDED

#include <stdint.h>

/* Free-running microsecond clock from DMTimer2. time_us wraps every ~71.6
 * minutes: compare its values only as differences, and only over short
 * spans. A stamp or deadline that may sit unused for half a wrap (35.8
 * min) and then be compared again must be a time_us64 value. */
void time_init(void);
/* Main loop (interrupt-safe, but IRQ code should use time_ticks). */
uint64_t time_us64(void);
uint32_t time_us(void);
/* Raw 24 MHz counter; safe from interrupts. */
uint32_t time_ticks(void);

#endif
