#ifndef I2C_H_INCLUDED
#define I2C_H_INCLUDED

#include <stdint.h>

/* I2C0 master, polled, ~100 kHz. */
void i2c0_init(void);
/* Writes n bytes to a 7-bit address; 0 on NACK or timeout. */
int i2c0_write(uint8_t addr, const uint8_t *data, unsigned n);
/* After a failed write: bytes sent before it failed, and the raw status. */
extern unsigned i2c0_sent;
extern uint32_t i2c0_status;

#endif
