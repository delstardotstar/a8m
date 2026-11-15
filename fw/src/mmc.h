#ifndef MMC_H_INCLUDED
#define MMC_H_INCLUDED

#include <stdint.h>

/* SD cards, 512-byte blocks, polled; main loop only. Reads of 8 blocks or
 * more are multi-block (by EDMA into 64-byte aligned cacheable buffers when
 * mmc_use_dma). */
#define MMC_UNITS   2
#define MMC_SOM     0   /* boot card (MMC0): read only unless mmc_som_writable */
#define MMC_FRONT   1   /* front card (MMC2) */

int mmc_init(int unit);
int mmc_ready(int unit);
/* Asks a ready card for its status; a card that doesn't answer is no
 * longer ready (pulled). */
int mmc_present(int unit);
int mmc_read(int unit, uint32_t lba, uint8_t *buf, uint32_t count);
int mmc_write(int unit, uint32_t lba, const uint8_t *buf, uint32_t count);
/* Boot card writes are refused while 0. Only two places set it, around
 * writes of their own names: update.c (Load Software: app.new, app,
 * app.bak) and cal.c (Calibrate: calnew.bin, cal.bin, cal.bak). */
extern int mmc_som_writable;
/* Why the last call failed, or 0. */
extern const char *mmc_error;
/* Bulk reads by EDMA (default off, EXTRA=-DMMC_DMA); cleared after a
 * failed DMA read. */
extern int mmc_use_dma;

#endif
