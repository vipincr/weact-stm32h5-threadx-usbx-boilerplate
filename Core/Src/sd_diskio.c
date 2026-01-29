/**
  ******************************************************************************
  * @file    sd_diskio.c
  * @brief   FatFs diskio driver for STM32 HAL SD card
  ******************************************************************************
  * This file implements the disk I/O functions required by FatFs to access
  * the SD card through the STM32 HAL SD driver.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "ff.h"
#include "diskio.h"
#include "sdmmc.h"
#include "logger.h"
#include <string.h>

/* Private defines -----------------------------------------------------------*/
#define SD_TIMEOUT      1000U
#define SD_DEFAULT_BLOCK_SIZE   512U

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status(BYTE pdrv)
{
    DSTATUS stat = STA_NOINIT;

    if (pdrv != 0)
    {
        return STA_NOINIT;  /* Only drive 0 (SD card) supported */
    }

    if (SDMMC1_IsInitialized())
    {
        stat = 0;  /* Disk is initialized and ready */
    }

    return stat;
}

/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize(BYTE pdrv)
{
    DSTATUS stat = STA_NOINIT;

    if (pdrv != 0)
    {
        return STA_NOINIT;  /* Only drive 0 (SD card) supported */
    }

    /* SD card should already be initialized by main.c
     * Just check if it's ready */
    if (SDMMC1_IsInitialized())
    {
        stat = 0;  /* Success */
    }
    else
    {
        /* Try to initialize */
        if (SDMMC1_SafeInit() == 0)
        {
            stat = 0;
        }
    }

    return stat;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT res = RES_ERROR;
    HAL_StatusTypeDef hal_status;

    if (pdrv != 0 || count == 0)
    {
        return RES_PARERR;
    }

    if (!SDMMC1_IsInitialized())
    {
        return RES_NOTRDY;
    }

    /* Read blocks from SD card */
    hal_status = HAL_SD_ReadBlocks(&hsd1, buff, (uint32_t)sector, count, SD_TIMEOUT);
    
    if (hal_status == HAL_OK)
    {
        /* Wait until transfer is complete */
        uint32_t wait_start = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - wait_start) > SD_TIMEOUT)
            {
                LOG_ERROR_TAG("DISKIO", "Timeout sector %lu", (unsigned long)sector);
                return RES_ERROR;
            }
        }
        
        res = RES_OK;
    }
    else
    {
        LOG_ERROR_TAG("DISKIO", "Read err sector %lu hal=%d", (unsigned long)sector, (int)hal_status);
    }

    return res;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s) - Only if FF_FS_READONLY == 0                         */
/*-----------------------------------------------------------------------*/
#if FF_FS_READONLY == 0

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    DRESULT res = RES_ERROR;

    if (pdrv != 0 || count == 0)
    {
        return RES_PARERR;
    }

    if (!SDMMC1_IsInitialized())
    {
        return RES_NOTRDY;
    }

    /* Write blocks to SD card */
    if (HAL_SD_WriteBlocks(&hsd1, (uint8_t *)buff, (uint32_t)sector, count, SD_TIMEOUT) == HAL_OK)
    {
        /* Wait until transfer is complete */
        uint32_t wait_start = HAL_GetTick();
        while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
        {
            if ((HAL_GetTick() - wait_start) > SD_TIMEOUT)
            {
                return RES_ERROR;
            }
        }
        res = RES_OK;
    }

    return res;
}

#endif /* FF_FS_READONLY == 0 */

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;
    HAL_SD_CardInfoTypeDef CardInfo;

    if (pdrv != 0)
    {
        return RES_PARERR;
    }

    if (!SDMMC1_IsInitialized())
    {
        return RES_NOTRDY;
    }

    switch (cmd)
    {
        case CTRL_SYNC:
            /* Make sure all data has been written */
            res = RES_OK;
            break;

        case GET_SECTOR_COUNT:
            if (HAL_SD_GetCardInfo(&hsd1, &CardInfo) == HAL_OK)
            {
                *(LBA_t *)buff = (LBA_t)CardInfo.BlockNbr;
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE:
            if (HAL_SD_GetCardInfo(&hsd1, &CardInfo) == HAL_OK)
            {
                *(WORD *)buff = (WORD)CardInfo.BlockSize;
                res = RES_OK;
            }
            break;

        case GET_BLOCK_SIZE:
            /* Erase block size in units of sector */
            *(DWORD *)buff = 1;  /* Unknown, use 1 sector */
            res = RES_OK;
            break;

        default:
            res = RES_PARERR;
            break;
    }

    return res;
}

/*-----------------------------------------------------------------------*/
/* Get current time for FAT timestamp                                    */
/*-----------------------------------------------------------------------*/
DWORD get_fattime(void)
{
    /* Return a fixed timestamp for read-only operation.
     * Format: bit[31:25] = Year-1980, bit[24:21] = Month, bit[20:16] = Day,
     *         bit[15:11] = Hour, bit[10:5] = Minute, bit[4:0] = Second/2
     */
    
    /* 2026-01-29 12:00:00 */
    return ((DWORD)(2026 - 1980) << 25)  /* Year = 2026 */
         | ((DWORD)1 << 21)               /* Month = January */
         | ((DWORD)29 << 16)              /* Day = 29 */
         | ((DWORD)12 << 11)              /* Hour = 12 */
         | ((DWORD)0 << 5)                /* Minute = 0 */
         | ((DWORD)0 >> 1);               /* Second = 0 */
}
