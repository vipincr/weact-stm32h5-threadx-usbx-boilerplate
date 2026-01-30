/**
  ******************************************************************************
  * @file    sd_adapter.h
  * @brief   SD Card Adapter - Simple unified access layer
  ******************************************************************************
  * Thin wrapper around HAL SD functions to:
  * - Provide single entry point for read/write
  * - Handle wait-for-ready in one place
  * - Track write source (MSC vs FatFS) for future coordination
  *
  * This is NOT thread-safe by design - the existing code works without
  * mutex protection because USB MSC and FatFS naturally don't collide
  * (when MSC is active, FatFS operations fail gracefully).
  ******************************************************************************
  */
#ifndef SD_ADAPTER_H
#define SD_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Write source tracking
  */
typedef enum {
    SD_SOURCE_NONE = 0,
    SD_SOURCE_FATFS,
    SD_SOURCE_MSC
} SD_Source_t;

/**
  * @brief  SD access mode - only one can be active at a time
  */
typedef enum {
    SD_MODE_FATFS = 0,   /**< FatFS has exclusive access, MSC reports no media */
    SD_MODE_MSC          /**< MSC has exclusive access, FatFS is unmounted */
} SD_Mode_t;

/**
  * @brief  Get current SD access mode.
  * @retval Current mode (SD_MODE_FATFS or SD_MODE_MSC)
  */
SD_Mode_t SD_GetMode(void);

/**
  * @brief  Set SD access mode.
  *         Caller is responsible for unmounting FatFS before switching to MSC
  *         and remounting FatFS after switching back.
  * @param  mode: New mode
  */
void SD_SetMode(SD_Mode_t mode);

/**
  * @brief  Check if MSC is allowed to access SD card.
  *         Returns true only when in MSC mode.
  * @retval 1 if MSC can access, 0 otherwise
  */
int SD_IsMscAllowed(void);

/**
  * @brief  Read sectors from SD card.
  * @param  buffer: Destination buffer
  * @param  sector: Starting sector (LBA)
  * @param  count: Number of sectors
  * @retval 0 on success, -1 on error
  */
int SD_Read(uint8_t *buffer, uint32_t sector, uint32_t count);

/**
  * @brief  Write sectors to SD card.
  * @param  buffer: Source buffer
  * @param  sector: Starting sector (LBA)
  * @param  count: Number of sectors
  * @param  source: Who is writing (for tracking)
  * @retval 0 on success, -1 on error
  */
int SD_Write(const uint8_t *buffer, uint32_t sector, uint32_t count, SD_Source_t source);

/**
  * @brief  Check if SD card is ready for operations.
  * @retval 1 if ready, 0 if not
  */
int SD_IsReady(void);

/**
  * @brief  Get who wrote last.
  * @retval Last write source
  */
SD_Source_t SD_GetLastWriteSource(void);

/**
  * @brief  Clear write source tracking (call when FatFS has synced).
  */
void SD_ClearWriteSource(void);

/**
  * @brief  Get SD card sector count.
  * @retval Number of sectors, or 0 if not ready
  */
uint32_t SD_GetSectorCount(void);

/**
  * @brief  Get SD card sector size.
  * @retval Sector size in bytes (typically 512)
  */
uint32_t SD_GetSectorSize(void);

/**
  * @brief  Set MSC active state.
  *         Call from MSC activate/deactivate callbacks.
  * @param  active: 1 when MSC is activated, 0 when deactivated
  */
void SD_SetMscActive(int active);

/**
  * @brief  Check if MSC is currently active.
  *         Uses activity-based timeout detection - if no MSC I/O for 3 seconds
  *         after activity was seen, considers MSC inactive (host ejected).
  * @retval 1 if MSC active, 0 otherwise
  */
int SD_IsMscActive(void);

/**
  * @brief  Notify that MSC had I/O activity.
  *         Call from MSC read/write callbacks to update activity timestamp.
  */
void SD_MscNotifyActivity(void);

/**
  * @brief  Get the tick of last MSC activity.
  *         Used to detect when host has stopped accessing the disk.
  * @retval HAL tick of last MSC read/write, or 0 if never accessed
  */
uint32_t SD_MscGetLastActivityTick(void);

/**
  * @brief  Set FatFS busy state.
  *         Call before/after FatFS operations to prevent MSC conflicts.
  * @param  busy: 1 when starting FatFS operation, 0 when done
  */
void SD_SetFatFsBusy(int busy);

/**
  * @brief  Check if FatFS is currently using the SD card.
  * @retval 1 if FatFS busy, 0 otherwise
  */
int SD_IsFatFsBusy(void);

/**
  * @brief  Set media changed flag.
  *         Triggers UNIT ATTENTION on next MSC status query.
  *         Does NOT set ejected state.
  */
void SD_SetMediaChanged(void);

/**
  * @brief  Set ejected state (called by SCSI eject handler).
  *         Only call this when host sends START_STOP_UNIT with eject bit.
  *         This sets both media_changed and media_ejected flags.
  */
void SD_SetEjected(void);

/**
  * @brief  Check and consume media changed flag.
  *         Returns 1 and clears flag if set, 0 otherwise.
  *         Use in USBD_STORAGE_Status to report UNIT ATTENTION once.
  * @retval 1 if media changed (flag was set), 0 otherwise
  */
int SD_ConsumeMediaChanged(void);

/**
  * @brief  Check if media was ejected by host.
  *         After host eject, media should be reported as "not present"
  *         until mode is explicitly changed.
  * @retval 1 if ejected, 0 otherwise
  */
int SD_IsEjected(void);

/**
  * @brief  Clear ejected state.
  *         Call when switching modes to reset ejected state.
  */
void SD_ClearEjected(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_ADAPTER_H */
