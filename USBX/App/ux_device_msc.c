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
volatile uint32_t g_msc_notification_count = 0U;
volatile uint32_t g_msc_read_count = 0U;
volatile uint32_t g_msc_write_count = 0U;

/* GET EVENT STATUS NOTIFICATION response buffer for removable media.
 * This tells macOS/Windows that media status hasn't changed.
 * Format: Event Header (4 bytes) + Media Event Descriptor (4 bytes)
 */
static UCHAR msc_notification_response[8] = {
  0x00, 0x02,  /* Event Descriptor Length (2 bytes after this) */
  0x04,        /* Notification Class (0x04 = Media) */
  0x00,        /* Supported Event Classes */
  0x02,        /* Media Event Code: 0x02 = Media Absent (no media present) */
  0x02,        /* Media Status: 0x02 = Media absent, door closed */
  0x00, 0x00   /* Start Slot, End Slot */
};

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static int32_t check_sd_status(void);
static int32_t ensure_sd_initialized(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static int32_t ensure_sd_initialized(void)
{
  /* SDMMC1_SafeInit() is idempotent and non-fatal. */
  return (SDMMC1_SafeInit() == 0) ? 0 : -1;
}

/**
  * @brief  check_sd_status
  *         check SD card Transfer Status.
  * @param  none
  * @retval BSP status
  */
static int32_t check_sd_status(void)
{
  if (ensure_sd_initialized() != 0)
  {
    return -1;
  }

  uint32_t start = HAL_GetTick();

  while (HAL_GetTick() - start < SD_TIMEOUT)
  {
    if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER)
    {
      return 0;
    }
  }

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
  if (ensure_sd_initialized() == 0)
  {
    LOG_INFO_TAG("MSC", "MSC activated - SD ready");
  }
  else
  {
    LOG_INFO_TAG("MSC", "MSC activated - SD not ready");
  }
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
  UINT status = UX_SUCCESS;

  /* USER CODE BEGIN USBD_STORAGE_Read */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(media_status);

  g_msc_read_count++;

  /* Check if SD card is ready */
  if (check_sd_status() != 0)
  {
    /* No SD card - set sense code but return SUCCESS.
     * Returning UX_ERROR causes aggressive retries from host.
     * The sense code tells host "Not Ready, Medium Not Present"
     */
    if (media_status != UX_NULL)
    {
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    return UX_SUCCESS;  /* Command handled, error is in sense code */
  }

  /* Read blocks from SD card */
  if (HAL_SD_ReadBlocks(&hsd1, data_pointer, lba, number_blocks, SD_TIMEOUT) != HAL_OK)
  {
    LOG_ERROR_TAG("MSC", "Read failed at LBA %lu", (unsigned long)lba);
    return UX_ERROR;
  }

  /* Wait until transfer is complete - WITH TIMEOUT */
  uint32_t wait_start = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - wait_start) > SD_TIMEOUT)
    {
      LOG_ERROR_TAG("MSC", "Read wait timeout at LBA %lu", (unsigned long)lba);
      return UX_ERROR;
    }
  }
  /* USER CODE END USBD_STORAGE_Read */

  return status;
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
  UINT status = UX_SUCCESS;

  /* USER CODE BEGIN USBD_STORAGE_Write */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(media_status);

  /* Check if SD card is ready */
  if (check_sd_status() != 0)
  {
    /* No SD card - set sense code but return SUCCESS.
     * Returning UX_ERROR causes aggressive retries from host.
     */
    if (media_status != UX_NULL)
    {
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    return UX_SUCCESS;  /* Command handled, error is in sense code */
  }

  /* Write blocks to SD card */
  if (HAL_SD_WriteBlocks(&hsd1, data_pointer, lba, number_blocks, SD_TIMEOUT) != HAL_OK)
  {
    LOG_ERROR_TAG("MSC", "Write failed at LBA %lu", (unsigned long)lba);
    return UX_ERROR;
  }

  /* Wait until transfer is complete - WITH TIMEOUT */
  uint32_t wait_start = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
  {
    if ((HAL_GetTick() - wait_start) > SD_TIMEOUT)
    {
      LOG_ERROR_TAG("MSC", "Write wait timeout at LBA %lu", (unsigned long)lba);
      return UX_ERROR;
    }
  }
  /* USER CODE END USBD_STORAGE_Write */

  return status;
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
  UINT status = UX_SUCCESS;

  /* USER CODE BEGIN USBD_STORAGE_Flush */
  UX_PARAMETER_NOT_USED(storage_instance);
  UX_PARAMETER_NOT_USED(lun);
  UX_PARAMETER_NOT_USED(number_blocks);
  UX_PARAMETER_NOT_USED(lba);
  UX_PARAMETER_NOT_USED(media_status);
  /* USER CODE END USBD_STORAGE_Flush */

  return status;
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
  
  /* Report actual SD card status to the host */
  if (ensure_sd_initialized() != 0)
  {
    /* No SD card present - report media not present */
    if (media_status != UX_NULL)
    {
      /* Not Ready (0x02), Medium Not Present (0x3A), No Qualifier (0x00) */
      *media_status = UX_DEVICE_CLASS_STORAGE_SENSE_STATUS(0x02, 0x3A, 0x00);
    }
    /* Still return SUCCESS - the error is in media_status */
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
  
  g_msc_notification_count++;
  
  /* Handle GET_EVENT_STATUS_NOTIFICATION - critical for macOS compatibility.
   * Without this, macOS will reset the USB device after repeated polling failures.
   */
  if (notification_class == 0x10U) /* 0x10 = Media class notification */
  {
    /* Update media status in response buffer based on SD card presence */
    if (ensure_sd_initialized() == 0)
    {
      /* Media present */
      msc_notification_response[4] = 0x00U; /* Media Event: No Change */
      msc_notification_response[5] = 0x01U; /* Media Status: Present, door closed */
    }
    else
    {
      /* Media absent */
      msc_notification_response[4] = 0x02U; /* Media Event: Media Absent */
      msc_notification_response[5] = 0x02U; /* Media Status: Absent, door closed */
    }
    
    *media_notification = msc_notification_response;
    *media_notification_length = sizeof(msc_notification_response);
  }
  else
  {
    /* Unknown notification class - return empty response */
    *media_notification = UX_NULL;
    *media_notification_length = 0U;
  }
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

  if (ensure_sd_initialized() != 0)
  {
    /* No media: report a minimal 1-block LUN so enumeration still works. */
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

  if (ensure_sd_initialized() != 0)
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

/* USER CODE END 1 */
