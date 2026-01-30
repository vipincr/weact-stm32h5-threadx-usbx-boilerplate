/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ux_device_msc.c
  * @author  MCD Application Team
  * @brief   USBX Device applicative file
  ******************************************************************************
   * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "ux_device_msc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "sdmmc.h"
#include "sd_adapter.h"
#include "logger.h"
#include "ux_device_class_storage.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SD_TIMEOUT     1000U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* Diagnostic counters for MSC callbacks */
volatile uint32_t g_msc_status_count = 0U;
volatile uint32_t g_msc_read_count = 0U;
volatile uint32_t g_msc_write_count = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static int32_t check_sd_status(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  check_sd_status
  *         Quick check if SD card is ready. Non-blocking.
  * @param  none
  * @retval 0 if ready, -1 if not
  */
static int32_t check_sd_status(void)
{
  /* Just check if already initialized */
  if (!SDMMC1_IsInitialized())
  {
    return -1;
  }

  /* Quick non-blocking check - just see if card is in transfer state now */
  if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER)
  {
    return 0;
  }

  /* Card busy - don't wait, just report not ready */
  return -1;
}
/* USER CODE END 0 */

/**
  * @brief  USBD_STORAGE_Activate
  *         This function is called when insertion of a storage device.
  * @param  storage_instance: Pointer to the storage class instance.
  * @retval none
  */
VOID USBD_STORAGE_Activate(VOID *storage_instance)
{
  /* USER CODE BEGIN USBD_STORAGE_Activate */
  UX_PARAMETER_NOT_USED(storage_instance);
  SD_SetMscActive(1);  /* Pause FatFS monitoring */
  LOG_INFO_TAG("MSC", "Activated (SD %s)", SDMMC1_IsInitialized() ? "ready" : "not ready");
  /* USER CODE END USBD_STORAGE_Activate */

  return;
}

/**
  * @brief  USBD_STORAGE_Deactivate
  *         This function is called when extraction of a storage device.
  * @param  storage_instance: Pointer to the storage class instance.
  * @retval none
  */
VOID USBD_STORAGE_Deactivate(VOID *storage_instance)
{
  /* USER CODE BEGIN USBD_STORAGE_Deactivate  */
  UX_PARAMETER_NOT_USED(storage_instance);
  SD_SetMscActive(0);  /* Resume FatFS monitoring */
  /* USER CODE END USBD_STORAGE_Deactivate */

  return;
}

/**
  * @brief  USBD_STORAGE_Read
  *         This function is invoked to read from media.
  * @param  storage_instance : Pointer to the storage class instance.
  * @param  lun: Logical unit number is the command is directed to.
  * @param  data_pointer: Address of the buffer to be used for reading or writing.
  * @param  number_blocks: number of sectors to read/write.
  * @param  lba: Logical block address is the sector address to read.
  * @param  media_status: should be filled out exactly like the media status
  *                       callback return value.
  * @retval status
  */
UINT USBD_STORAGE_Read(VOID *storage_instance, ULONG lun, UCHAR *data_pointer,
                       ULONG number_blocks, ULONG lba, ULONG *media_status)
{
  /* USER CODE BEGIN USBD_STORAGE_Read */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(media_status);

  g_msc_read_count++;

  /* Only allow MSC access when in MSC mode.
   * In FatFS mode, report "no media" so host sees disk as ejected. */
  if (!SD_IsMscAllowed())
  {
    if (media_status != UX_NULL)
    {
      /* Not Ready, Medium Not Present */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    return UX_ERROR;
  }

  /* Check if SD card is ready */
  if (check_sd_status() != 0)
  {
    /* No SD card - set sense code. */
    if (media_status != UX_NULL)
    {
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    return UX_ERROR;
  }

  /* Notify activity for idle timeout detection */
  SD_MscNotifyActivity();

  /* Use SD adapter for read */
  if (SD_Read(data_pointer, lba, number_blocks) != 0)
  {
    LOG_ERROR_TAG("MSC", "Read failed at LBA %lu", (unsigned long)lba);
    return UX_ERROR;
  }

  /* USER CODE END USBD_STORAGE_Read */

  return UX_SUCCESS;
}

/**
  * @brief  USBD_STORAGE_Write
  *         This function is invoked to write in media.
  * @param  storage_instance : Pointer to the storage class instance.
  * @param  lun: Logical unit number is the command is directed to.
  * @param  data_pointer: Address of the buffer to be used for reading or writing.
  * @param  number_blocks: number of sectors to read/write.
  * @param  lba: Logical block address is the sector address to read.
  * @param  media_status: should be filled out exactly like the media status
  *                       callback return value.
  * @retval status
  */
UINT USBD_STORAGE_Write(VOID *storage_instance, ULONG lun, UCHAR *data_pointer,
                        ULONG number_blocks, ULONG lba, ULONG *media_status)
{
  /* USER CODE BEGIN USBD_STORAGE_Write */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(media_status);

  g_msc_write_count++;

  /* Only allow MSC access when in MSC mode. */
  if (!SD_IsMscAllowed())
  {
    if (media_status != UX_NULL)
    {
      /* Not Ready, Medium Not Present */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    return UX_ERROR;
  }

  /* Check if SD card is ready */
  if (check_sd_status() != 0)
  {
    /* No SD card - set sense code. */
    if (media_status != UX_NULL)
    {
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    return UX_ERROR;
  }

  /* Notify activity for idle timeout detection */
  SD_MscNotifyActivity();

  /* Use SD adapter for write (mark as MSC source) */
  if (SD_Write(data_pointer, lba, number_blocks, SD_SOURCE_MSC) != 0)
  {
    LOG_ERROR_TAG("MSC", "Write failed at LBA %lu", (unsigned long)lba);
    return UX_ERROR;
  }

  /* USER CODE END USBD_STORAGE_Write */

  return UX_SUCCESS;
}

/**
  * @brief  USBD_STORAGE_Flush
  *         This function is invoked to flush media.
  * @param  storage_instance : Pointer to the storage class instance.
  * @param  lun: Logical unit number is the command is directed to.
  * @param  number_blocks: number of sectors to read/write.
  * @param  lba: Logical block address is the sector address to read.
  * @param  media_status: should be filled out exactly like the media status
  *                       callback return value.
  * @retval status
  */
UINT USBD_STORAGE_Flush(VOID *storage_instance, ULONG lun, ULONG number_blocks,
                        ULONG lba, ULONG *media_status)
{
  /* USER CODE BEGIN USBD_STORAGE_Flush */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(number_blocks);
  UX_PARAMETER_NOT_USED(lba);
  UX_PARAMETER_NOT_USED(media_status);
  
  /* ThreadX (RTOS) mode expects UX_SUCCESS for success */
  /* USER CODE END USBD_STORAGE_Flush */

  return UX_SUCCESS;
}

/**
  * @brief  USBD_STORAGE_Status
  *         This function is invoked to obtain the status of the device.
  * @param  storage_instance : Pointer to the storage class instance.
  * @param  lun: Logical unit number is the command is directed to.
  * @param  media_id: is not currently used.
  * @param  media_status: should be filled out exactly like the media status
  *                       callback return value.
  * @retval status
  */
UINT USBD_STORAGE_Status(VOID *storage_instance, ULONG lun, ULONG media_id,
                         ULONG *media_status)
{
  UINT status = UX_SUCCESS;

  /* USER CODE BEGIN USBD_STORAGE_Status */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(media_id);
  
  g_msc_status_count++;
  
  /* Check for media changed flag first - return UNIT ATTENTION once to tell host
   * "something changed, re-query me". This is the SCSI mechanism for media removal.
   * Sense Key 0x06 = UNIT ATTENTION, ASC 0x28 = NOT READY TO READY CHANGE */
  if (SD_ConsumeMediaChanged())
  {
    /* Don't log here - called from USBX thread, logging to CDC causes deadlock */
    if (media_status != UX_NULL)
    {
      /* UNIT ATTENTION: Media may have changed - forces host to unmount */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x06, 0x28, 0x00);
    }
    status = UX_ERROR;
  }
  /* If host requested eject, report "no media" until mode changes */
  else if (SD_IsEjected())
  {
    if (media_status != UX_NULL)
    {
      /* NOT READY: Medium Not Present */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    status = UX_ERROR;
  }
  /* In FatFS mode, report "no media" so host doesn't try to access disk */
  else if (!SD_IsMscAllowed())
  {
    if (media_status != UX_NULL)
    {
      /* NOT READY: Medium Not Present */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    status = UX_ERROR;
  }
  /* Check if SD card is actually initialized */
  else if (!SDMMC1_IsInitialized())
  {
    /* No SD card present - report NOT READY to host. */
    if (media_status != UX_NULL)
    {
      /* Not Ready (0x02), Medium Not Present (0x3A), No Qualifier (0x00) */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    status = UX_ERROR;
  }
  else
  {
    if (media_status != UX_NULL)
    {
      *media_status = 0U; /* Media OK */
    }
  }
  /* USER CODE END USBD_STORAGE_Status */

  return status;
}

/**
  * @brief  USBD_STORAGE_Notification
  *         This function is invoked to obtain the notification of the device.
  * @param  storage_instance : Pointer to the storage class instance.
  * @param  lun: Logical unit number is the command is directed to.
  * @param  media_id: is not currently used.
  * @param  notification_class: specifies the class of notification.
  * @param  media_notification: response for the notification.
  * @param  media_notification_length: length of the response buffer.
  * @retval status
  */
UINT USBD_STORAGE_Notification(VOID *storage_instance, ULONG lun, ULONG media_id,
                               ULONG notification_class, UCHAR **media_notification,
                               ULONG *media_notification_length)
{
  UINT status = UX_SUCCESS;

  /* USER CODE BEGIN USBD_STORAGE_Notification */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(media_id);
  UX_PARAMETER_NOT_USED(notification_class);
  
  /* Notification not needed - MSC and FatFS are mutually exclusive.
   * Return empty response for all notification requests. */
  *media_notification = UX_NULL;
  *media_notification_length = 0U;
  /* USER CODE END USBD_STORAGE_Notification */

  return status;
}

/**
  * @brief  USBD_STORAGE_GetMediaLastLba
  *         Get Media last LBA.
  * @param  none
  * @retval last lba
  */
ULONG USBD_STORAGE_GetMediaLastLba(VOID)
{
  ULONG LastLba = 0U;

  /* USER CODE BEGIN USBD_STORAGE_GetMediaLastLba */
  HAL_SD_CardInfoTypeDef CardInfo;

  if (!SDMMC1_IsInitialized())
  {
    /* No media: report 0 blocks. */
    return 0U;
  }

  if (HAL_SD_GetCardInfo(&hsd1, &CardInfo) != HAL_OK)
  {
    return 0U;
  }

  LastLba = (ULONG)(CardInfo.BlockNbr - 1U);
  /* USER CODE END USBD_STORAGE_GetMediaLastLba */

  return LastLba;
}

/**
  * @brief  USBD_STORAGE_GetMediaBlocklength
  *         Get Media block length.
  * @param  none.
  * @retval block length.
  */
ULONG USBD_STORAGE_GetMediaBlocklength(VOID)
{
  ULONG MediaBlockLen = 0U;

  /* USER CODE BEGIN USBD_STORAGE_GetMediaBlocklength */
  HAL_SD_CardInfoTypeDef CardInfo;

  if (!SDMMC1_IsInitialized())
  {
    return 512U;
  }

  if (HAL_SD_GetCardInfo(&hsd1, &CardInfo) != HAL_OK)
  {
    return 512U;
  }

  MediaBlockLen = (ULONG)CardInfo.BlockSize;
  /* USER CODE END USBD_STORAGE_GetMediaBlocklength */

  return MediaBlockLen;
}

/* USER CODE BEGIN 1 */

/**
  * @brief  USBD_STORAGE_EjectNotify
  *         Called by USBX when host sends START_STOP_UNIT with eject bit.
  *         This is a weak function called from the modified ux_device_class_storage_start_stop.c
  *         WARNING: This runs in USBX thread context - do NOT log or block here!
  * @retval none
  */
void USBD_STORAGE_EjectNotify(void)
{
  /* Just set the flag - don't log from USBX callback context!
   * Logging to CDC from the storage thread causes deadlock. */
  SD_SetEjected();
}

/* USER CODE END 1 */
