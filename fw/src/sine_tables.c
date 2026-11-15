/*
 * The mdkit voices' sine functions, each a pure function of a 15-bit
 * index, evaluated once into tables (the voices did a double-precision
 * series per sample). The three formulas are copied verbatim from
 * efm/cy, trx/xt and trx/bd; the voices that share each table were
 * checked to produce identical values (fw/mdkit/LOCAL_CHANGES).
 */
#include "mdkit_tables.h"

uint32_t mdkit_sine_efm[MDKIT_SINE_SIZE];
uint32_t mdkit_sine_xt[MDKIT_SINE_SIZE];
uint32_t mdkit_sine_trx[MDKIT_SINE_SIZE];

#define MDKIT_MASK24 UINT32_C(0xffffff)
#define MDKIT_TWO_PI 6.283185307179586476925286766559
#define MDKIT_PI     3.14159265358979323846

static uint32_t efm_sine(uint16_t index)
{
    double x = (MDKIT_TWO_PI * (double)(index & 0x7fffu)) / 32768.0;
    x -= MDKIT_TWO_PI * (double)(long long)(x / MDKIT_TWO_PI
                                               + (x < 0.0 ? -0.5 : 0.5));
    if (x >  MDKIT_PI / 2.0) x =  MDKIT_PI - x;
    else if (x < -MDKIT_PI / 2.0) x = -MDKIT_PI - x;
    const double x2 = x * x;
    const double s = x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0
        - x2 * (1.0 / 5040.0 - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0
        - x2 * (1.0 / 6227020800.0 - x2 * (1.0 / 1307674368000.0))))))));
    const double scaled = 8388608.0 * s;
    long long sample = (long long)(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    if (sample > 0x7fffff) sample = 0x7fffff;
    if (sample < -0x800000) sample = -0x800000;
    return (uint32_t)sample & MDKIT_MASK24;
}

static double xt_local_sin(double x)
{
    x -= 6.283185307179586476925286766559 * (double)(long long)(x / 6.283185307179586476925286766559
                                            + (x < 0.0 ? -0.5 : 0.5));
    if (x > 3.14159265358979323846 / 2.0) x = 3.14159265358979323846 - x;
    else if (x < -3.14159265358979323846 / 2.0) x = -3.14159265358979323846 - x;
    const double x2 = x * x;
    return x * (1.0 - x2 * (1.0 / 6.0 - x2 * (1.0 / 120.0 - x2 * (1.0 / 5040.0
        - x2 * (1.0 / 362880.0 - x2 * (1.0 / 39916800.0))))));
}

static uint32_t xt_local_sine(uint16_t index){double y=8388608.0*xt_local_sin(6.283185307179586476925286766559*(double)(index&32767u)/32768.0);int64_t v=(int64_t)(y+(y<0?-0.5:0.5));if(v>8388607)v=8388607;if(v< -8388608)v=-8388608;return (uint32_t)v&MDKIT_MASK24;}

static uint32_t trx_local_sine(unsigned index) {
 const double pi=3.1415926535897932384626433832795;
 double x=(double)(index&32767u)*(2.0*pi/32768.0);
 if(x>pi)x-=2.0*pi;
 if(x>pi/2.0)x=pi-x;else if(x< -pi/2.0)x=-pi-x;
 double z=x*x;
 double y=x*(1.0-z*(1.0/6.0-z*(1.0/120.0-z*(1.0/5040.0-z*(1.0/362880.0-z*(1.0/39916800.0-z/6227020800.0))))));
 double a=y*8388608.0;
 int64_t q=(int64_t)(a<0?a-0.5:a+0.5);
 if(q>8388607)q=8388607;
 if(q< -8388608)q=-8388608;
 return (uint32_t)q&0xffffffu;
}

uint32_t mdkit_model_memory[MDKIT_MODEL_MEMORY_SIZE];

void mdkit_model_memory_fill(uint32_t (*calc)(unsigned))
{
    static int filled;

    if (!filled) {
        for (unsigned i = 0; i < MDKIT_MODEL_MEMORY_SIZE; i++) {
            mdkit_model_memory[i] = calc(i);
        }
        filled = 1;
    }
}

uint32_t mdkit_metal_rom[MDKIT_METAL_ROM_SIZE];

void mdkit_metal_rom_fill(uint32_t (*calc)(uint32_t))
{
    static int filled;

    if (!filled) {
        for (uint32_t i = 0; i < MDKIT_METAL_ROM_SIZE; i++) {
            mdkit_metal_rom[i] = calc(MDKIT_METAL_ROM_BASE + i);
        }
        filled = 1;
    }
}

void mdkit_tables_init(void)
{
    for (unsigned i = 0; i < MDKIT_SINE_SIZE; i++) {
        mdkit_sine_efm[i] = efm_sine((uint16_t)i);
        mdkit_sine_xt[i] = xt_local_sine((uint16_t)i);
        mdkit_sine_trx[i] = trx_local_sine(i);
    }
}
