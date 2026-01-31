/**
  ******************************************************************************
  * @file    sd_adapter.c
  * @brief   SD Card Adapter - Simple unified access layer
  ******************************************************************************
  * Thin wrapper around HAL SD functions. Centralizes:
  * - Wait-for-ready logic with timeout
  * - Error handling (graceful failures, no blocking)
  * - Write source tracking
  * - MSC/FatFS coordination (flags only, no mutex)
  *
  * Design: NO MUTEX - allow concurrent access attempts. When MSC and FatFS
  * collide, one will timeout gracefully. The fs_reader handles disk errors
  * by skipping the monitoring cycle (has_error flag).
  ******************************************************************************
  */

#include "sd_adapter.h"
#include "sdmmc.h"
#include "stm32h5xx_hal.h"

/* Private defines -----------------------------------------------------------*/
#define SD_TIMEOUT_MS       1000U

/* Private variables ---------------------------------------------------------*/
static volatile SD_Source_t last_write_source = SD_SOURCE_NONE;
static volatile SD_Mode_t current_mode = SD_MODE_FATFS;  /* Start in FatFS mode */
static volatile uint8_t msc_activated = 0U;       /* Set by USBX activate callback */
static volatile uint8_t fatfs_busy = 0U;          /* Set when FatFS is accessing SD */
static volatile uint32_t msc_last_activity_tick = 0U;  /* Last MSC read/write tick */
static volatile uint8_t media_changed = 0U;       /* Set when mode changes to trigger UNIT ATTENTION */
static volatile uint8_t media_ejected = 0U;       /* Set when host requests eject */

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Wait for SD card to be in transfer state.
  * @retval 0 if ready, -1 on timeout
  */
static int wait_for_transfer_ready(void)
{
    uint32_t start = HAL_GetTick();
    
    while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
    {
        if ((HAL_GetTick() - start) > SD_TIMEOUT_MS)
        {
            return -1;
        }
    }
    return 0;
}

/* Public functions ----------------------------------------------------------*/

int SD_Read(uint8_t *buffer, uint32_t sector, uint32_t count)
{
    if (buffer == NULL || count == 0U)
    {
        return -1;
    }
    
    if (!SDMMC1_IsInitialized())
    {
        return -1;
    }
    
    /* Wait for card to be ready before starting */
    if (wait_for_transfer_ready() != 0)
    {
        return -1;
    }
    
    /* Perform read */
    if (HAL_SD_ReadBlocks(&hsd1, buffer, sector, count, SD_TIMEOUT_MS) != HAL_OK)
    {
        return -1;
    }
    
    /* Wait for completion */
    if (wait_for_transfer_ready() != 0)
    {
        return -1;
    }
    
    return 0;
}

int SD_Write(const uint8_t *buffer, uint32_t sector, uint32_t count, SD_Source_t source)
{
    if (buffer == NULL || count == 0U)
    {
        return -1;
    }
    
    if (!SDMMC1_IsInitialized())
    {
        return -1;
    }
    
    /* Wait for card to be ready before starting */
    if (wait_for_transfer_ready() != 0)
    {
        return -1;
    }
    
    /* Perform write */
    if (HAL_SD_WriteBlocks(&hsd1, (uint8_t *)buffer, sector, count, SD_TIMEOUT_MS) != HAL_OK)
    {
        return -1;
    }
    
    /* Wait for completion */
    if (wait_for_transfer_ready() != 0)
    {
        return -1;
    }
    
    /* Track write source */
    last_write_source = source;
    
    return 0;
}

int SD_IsReady(void)
{
    if (!SDMMC1_IsInitialized())
    {
        return 0;
    }
    
    /* Quick check - is card in transfer state? */
    return (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER) ? 1 : 0;
}

SD_Source_t SD_GetLastWriteSource(void)
{
    return last_write_source;
}

void SD_ClearWriteSource(void)
{
    last_write_source = SD_SOURCE_NONE;
}

uint32_t SD_GetSectorCount(void)
{
    HAL_SD_CardInfoTypeDef info;
    
    if (!SDMMC1_IsInitialized())
    {
        return 0;
    }
    
    if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK)
    {
        return 0;
    }
    
    return info.BlockNbr;
}

uint32_t SD_GetSectorSize(void)
{
    HAL_SD_CardInfoTypeDef info;
    
    if (!SDMMC1_IsInitialized())
    {
        return 512U;  /* Default */
    }
    
    if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK)
    {
        return 512U;
    }
    
    return info.BlockSize;
}

void SD_SetMscActive(int active)
{
    msc_activated = active ? 1U : 0U;
}

int SD_IsMscActive(void)
{
    return msc_activated ? 1 : 0;
}

void SD_MscNotifyActivity(void)
{
    msc_last_activity_tick = HAL_GetTick();
}

uint32_t SD_MscGetLastActivityTick(void)
{
    return msc_last_activity_tick;
}

void SD_SetFatFsBusy(int busy)
{
    fatfs_busy = busy ? 1U : 0U;
}

int SD_IsFatFsBusy(void)
{
    return fatfs_busy ? 1 : 0;
}

void SD_SetMediaChanged(void)
{
    media_changed = 1U;
    /* NOTE: Do NOT set media_ejected here!
     * Ejected flag is ONLY set by USBD_STORAGE_EjectNotify() 
     * when host sends SCSI START_STOP_UNIT with eject bit. */
}

void SD_SetEjected(void)
{
    media_ejected = 1U;
    media_changed = 1U;  /* Eject also triggers UNIT ATTENTION */
}

int SD_ConsumeMediaChanged(void)
{
    if (media_changed)
    {
        media_changed = 0U;
        return 1;
    }
    return 0;
}

int SD_IsEjected(void)
{
    return media_ejected ? 1 : 0;
}

void SD_ClearEjected(void)
{
    media_ejected = 0U;
    media_changed = 0U;
}

SD_Mode_t SD_GetMode(void)
{
    return current_mode;
}

void SD_SetMode(SD_Mode_t mode)
{
    current_mode = mode;
}

int SD_IsMscAllowed(void)
{
    return (current_mode == SD_MODE_MSC) ? 1 : 0;
}