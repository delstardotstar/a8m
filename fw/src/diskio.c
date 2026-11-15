/* FatFs media layer: drive 0 = SOM boot card (read only unless
 * mmc_som_writable), 1 = front card. */
#include "ff.h"
#include "diskio.h"
#include "mmc.h"

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv >= MMC_UNITS || !mmc_ready(pdrv)) {
        return STA_NOINIT;
    }
    return pdrv == MMC_SOM && !mmc_som_writable ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv >= MMC_UNITS) {
        return STA_NOINIT;
    }
    if (!mmc_ready(pdrv) && !mmc_init(pdrv)) {
        return STA_NOINIT;
    }
    return pdrv == MMC_SOM && !mmc_som_writable ? STA_PROTECT : 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    return pdrv < MMC_UNITS && mmc_read(pdrv, sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv == MMC_SOM && !mmc_som_writable) {
        return RES_WRPRT;
    }
    return pdrv < MMC_UNITS && mmc_write(pdrv, sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)buff;
    if (pdrv >= MMC_UNITS) {
        return RES_PARERR;
    }
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
