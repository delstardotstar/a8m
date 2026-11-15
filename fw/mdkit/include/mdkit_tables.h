/* Local addition (see ../LOCAL_CHANGES): shared 15-bit sine tables, filled
 * once by mdkit_tables_init() with the voices' own formulas. */
#ifndef MDKIT_TABLES_H
#define MDKIT_TABLES_H
#include <stdint.h>

#define MDKIT_SINE_SIZE 32768u

/* EFM SD RS HH CP CY CB (degree-17 series, wrapped) */
extern uint32_t mdkit_sine_efm[MDKIT_SINE_SIZE];
/* EFM XT, TRX XT B2 XC (degree-11 series, wrapped) */
extern uint32_t mdkit_sine_xt[MDKIT_SINE_SIZE];
/* TRX SD BD RS CL CB (degree-13 series) */
extern uint32_t mdkit_sine_trx[MDKIT_SINE_SIZE];

void mdkit_tables_init(void);

/* TRX voices' "model memory" (CB CH CL CP CY MA OH RS SD, all identical):
 * filled on the first such voice's init with its own function. */
#define MDKIT_MODEL_MEMORY_SIZE 8448u
extern uint32_t mdkit_model_memory[MDKIT_MODEL_MEMORY_SIZE];
void mdkit_model_memory_fill(uint32_t (*calc)(unsigned));

/* PI-RC/CC/HH ROM (identical in the three), 0x140000-0x14ffff: filled on
 * the first such voice's init with its own function. */
#define MDKIT_METAL_ROM_BASE 0x140000u
#define MDKIT_METAL_ROM_SIZE 0x10000u
extern uint32_t mdkit_metal_rom[MDKIT_METAL_ROM_SIZE];
void mdkit_metal_rom_fill(uint32_t (*calc)(uint32_t));

#endif
