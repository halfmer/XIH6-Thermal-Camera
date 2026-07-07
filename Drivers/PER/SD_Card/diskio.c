/*
 * diskio.c - FatFS disk I/O glue for the SD card (SDMMC2).
 *
 * DISABLED for now: this project does not include the FatFS middleware
 * (ff.c / ff.h / diskio.h / ffconf.h are not part of the build), so the FatFS
 * types used below (DRESULT / BYTE / DWORD / UINT / RES_*) are undefined and
 * this file would not compile. It is guarded with #if 0 so it can stay in the
 * project without breaking the build.
 *
 * The raw block driver in SD_Card.c (SD_ReadDisk_DMA / SD_WriteDisk_DMA) works
 * on its own. To add a filesystem later:
 *   1) add the FatFS middleware to the project (ff.c, diskio.h, ffconf.h, ...),
 *   2) remove the "#if 0" guard below,
 *   3) implement disk_initialize / disk_status / disk_ioctl / get_fattime too.
 */
#if 0

#include "diskio.h"
#include "SD_Card.h"

DRESULT disk_read (
    BYTE pdrv,     /* physical drive number (0: SD card) */
    BYTE *buff,    /* data buffer to store read data */
    DWORD sector,  /* start sector in LBA */
    UINT count     /* number of sectors to read */
)
{
    if (pdrv != 0 || buff == NULL || count == 0) return RES_PARERR;
    return (SD_ReadDisk_DMA(buff, sector, count) == SD_OK) ? RES_OK : RES_ERROR;
}

DRESULT disk_write (
    BYTE pdrv,          /* physical drive number */
    const BYTE *buff,   /* data to be written */
    DWORD sector,       /* start sector in LBA */
    UINT count          /* number of sectors to write */
)
{
    if (pdrv != 0 || buff == NULL || count == 0) return RES_PARERR;
    return (SD_WriteDisk_DMA(buff, sector, count) == SD_OK) ? RES_OK : RES_ERROR;
}

#endif /* 0 */
