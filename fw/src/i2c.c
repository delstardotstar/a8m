#include "i2c.h"
#include "diag.h"
#include "hw.h"

#define I2C0            0x44e0b000u
#define I2C_SYSC        0x010u
#define I2C_IRQSTATUS_RAW 0x024u
#define I2C_IRQSTATUS   0x028u
#define I2C_IRQENABLE_CLR 0x030u
#define I2C_SYSS        0x090u
#define I2C_CNT         0x098u
#define I2C_DATA        0x09cu
#define I2C_CON         0x0a4u
#define I2C_SA          0x0acu
#define I2C_PSC         0x0b0u
#define I2C_SCLL        0x0b4u
#define I2C_SCLH        0x0b8u
#define CM_WKUP_I2C0    0x0b8u

#define CON_EN          (1u << 15)
#define CON_MST         (1u << 10)
#define CON_TRX         (1u << 9)
#define CON_STP         (1u << 1)
#define CON_STT         (1u << 0)
#define ST_NACK         (1u << 1)
#define ST_ARDY         (1u << 2)
#define ST_XRDY         (1u << 4)
#define ST_BF           (1u << 8)
#define ST_BB           (1u << 12)
#define TIMEOUT         1000000u

unsigned i2c0_sent;
uint32_t i2c0_status;

void i2c0_init(void)
{
    clock_require(CM_WKUP, CM_WKUP_I2C0, CLK_ENABLE);
    REG(I2C0 + I2C_SYSC) = 0x2;                     /* soft reset */
    REG(I2C0 + I2C_CON) = CON_EN;
    for (uint32_t n = 0; n < TIMEOUT && !(REG(I2C0 + I2C_SYSS) & 1u); n++) {
    }
    REG(I2C0 + I2C_CON) = 0;
    /* 48 MHz / (3 + 1) = 12 MHz internal; SCL ~100 kHz (stock values). */
    REG(I2C0 + I2C_PSC) = 3;
    REG(I2C0 + I2C_SCLL) = 0x35;
    REG(I2C0 + I2C_SCLH) = 0x37;
    REG(I2C0 + I2C_IRQENABLE_CLR) = 0xffffffffu;
    REG(I2C0 + I2C_CON) = CON_EN;
}

static int wait(uint32_t mask)
{
    for (uint32_t n = 0; n < TIMEOUT; n++) {
        uint32_t st = REG(I2C0 + I2C_IRQSTATUS_RAW);
        if (st & ST_NACK) {
            return 0;
        }
        if (st & mask) {
            return 1;
        }
    }
    return 0;
}

int i2c0_write(uint8_t addr, const uint8_t *data, unsigned n)
{
    int ok = 1;

    for (uint32_t t = 0; t < TIMEOUT && (REG(I2C0 + I2C_IRQSTATUS_RAW) & ST_BB); t++) {
    }
    REG(I2C0 + I2C_IRQSTATUS) = 0xffffffffu;
    REG(I2C0 + I2C_SA) = addr;
    REG(I2C0 + I2C_CNT) = n;
    REG(I2C0 + I2C_CON) = CON_EN | CON_MST | CON_TRX | CON_STT | CON_STP;
    i2c0_sent = 0;
    for (unsigned i = 0; i < n && ok; i++) {
        ok = wait(ST_XRDY);
        if (ok) {
            REG(I2C0 + I2C_DATA) = data[i];
            REG(I2C0 + I2C_IRQSTATUS) = ST_XRDY;
            i2c0_sent++;
        }
    }
    ok = ok && wait(ST_ARDY);
    i2c0_status = REG(I2C0 + I2C_IRQSTATUS_RAW);
    if (!ok) {
        REG(I2C0 + I2C_CON) = CON_EN | CON_MST | CON_STP;
    }
    wait(ST_BF);
    REG(I2C0 + I2C_IRQSTATUS) = 0xffffffffu;
    return ok;
}
