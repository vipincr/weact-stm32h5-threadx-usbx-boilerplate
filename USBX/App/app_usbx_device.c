/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_usbx_device.c
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
#include "app_usbx_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "usb.h"
#include "logger.h"
#include "led_status.h"

#if defined(USBX_STANDALONE_BRINGUP)
#include "stm32h5xx_hal.h"
#endif
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

static ULONG storage_interface_number;
static ULONG storage_configuration_number;
static UX_SLAVE_CLASS_STORAGE_PARAMETER storage_parameter;
static ULONG cdc_acm_interface_number;
static ULONG cdc_acm_configuration_number;
static UX_SLAVE_CLASS_CDC_ACM_PARAMETER cdc_acm_parameter;
static TX_THREAD ux_device_app_thread;

/* USER CODE BEGIN PV */
extern PCD_HandleTypeDef hpcd_USB_DRD_FS;
extern volatile uint32_t g_usb_pcd_irq_count;
extern volatile uint32_t g_usb_reset_count;
extern volatile uint32_t g_usb_setup_count;
extern volatile uint32_t g_usb_set_config_count;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static VOID app_ux_device_thread_entry(ULONG thread_input);
/* USER CODE BEGIN PFP */

/* Some USBX builds include standalone task pumping; others don't.
 * Use a weak reference so we can call it only when linked in.
 */
extern UINT _ux_system_tasks_run(VOID) __attribute__((weak));

/* USER CODE END PFP */

/**
  * @brief  Application USBX Device Initialization.
  * @param  memory_ptr: memory pointer
  * @retval status
  */

UINT MX_USBX_Device_Init(VOID *memory_ptr)
{
   UINT ret = UX_SUCCESS;
  UCHAR *device_framework_high_speed;
  UCHAR *device_framework_full_speed;
  ULONG device_framework_hs_length;
  ULONG device_framework_fs_length;
  ULONG string_framework_length;
  ULONG language_id_framework_length;
  UCHAR *string_framework;
  UCHAR *language_id_framework;

  UCHAR *pointer;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;

  /* USER CODE BEGIN MX_USBX_Device_Init0 */

  /* USER CODE END MX_USBX_Device_Init0 */
  /* Allocate the stack for USBX Memory */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer,
                       USBX_DEVICE_MEMORY_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    /* USER CODE BEGIN USBX_ALLOCATE_STACK_ERROR */
    LED_FatalStageCode(1U, 1U);
    /* USER CODE END USBX_ALLOCATE_STACK_ERROR */
  }

  /* Initialize USBX Memory */
  if (ux_system_initialize(pointer, USBX_DEVICE_MEMORY_STACK_SIZE, UX_NULL, 0) != UX_SUCCESS)
  {
    /* USER CODE BEGIN USBX_SYSTEM_INITIALIZE_ERROR */
    LED_FatalStageCode(2U, 1U);
    /* USER CODE END USBX_SYSTEM_INITIALIZE_ERROR */
  }

  /* Get Device Framework High Speed and get the length */
  device_framework_high_speed = USBD_Get_Device_Framework_Speed(USBD_HIGH_SPEED,
                                                                &device_framework_hs_length);

  /* Get Device Framework Full Speed and get the length */
  device_framework_full_speed = USBD_Get_Device_Framework_Speed(USBD_FULL_SPEED,
                                                                &device_framework_fs_length);

  /* Get String Framework and get the length */
  string_framework = USBD_Get_String_Framework(&string_framework_length);

  /* Get Language Id Framework and get the length */
  language_id_framework = USBD_Get_Language_Id_Framework(&language_id_framework_length);

  /* Install the device portion of USBX */
  ret = ux_device_stack_initialize(device_framework_high_speed,
                                  device_framework_hs_length,
                                  device_framework_full_speed,
                                  device_framework_fs_length,
                                  string_framework,
                                  string_framework_length,
                                  language_id_framework,
                                  language_id_framework_length,
                                  UX_NULL);
  if (ret != UX_SUCCESS)
  {
    /* USER CODE BEGIN USBX_DEVICE_INITIALIZE_ERROR */
    LED_FatalStageCode(3U, (uint8_t)ret);
    /* USER CODE END USBX_DEVICE_INITIALIZE_ERROR */
  }

  /* Initialize the cdc acm class parameters for the device */
  cdc_acm_parameter.ux_slave_class_cdc_acm_instance_activate   = USBD_CDC_ACM_Activate;
  cdc_acm_parameter.ux_slave_class_cdc_acm_instance_deactivate = USBD_CDC_ACM_Deactivate;
  cdc_acm_parameter.ux_slave_class_cdc_acm_parameter_change    = USBD_CDC_ACM_ParameterChange;

  /* USER CODE BEGIN CDC_ACM_PARAMETER */
  /* Keep parameters standard. Logger hook is done via callback implementation below. */
  /* USER CODE END CDC_ACM_PARAMETER */

  /* Get cdc acm configuration number */
  cdc_acm_configuration_number = USBD_Get_Configuration_Number(CLASS_TYPE_CDC_ACM, 0);

  /* Find cdc acm interface number */
  cdc_acm_interface_number = USBD_Get_Interface_Number(CLASS_TYPE_CDC_ACM, 0);

  /* Initialize the device cdc acm class */
  if (ux_device_stack_class_register(_ux_system_slave_class_cdc_acm_name,
                                     ux_device_class_cdc_acm_entry,
                                     cdc_acm_configuration_number,
                                     cdc_acm_interface_number,
                                     &cdc_acm_parameter) != UX_SUCCESS)
  {
    /* USER CODE BEGIN USBX_DEVICE_CDC_ACM_REGISTER_ERROR */
    LED_FatalStageCode(4U, 1U);
    /* USER CODE END USBX_DEVICE_CDC_ACM_REGISTER_ERROR */
  }

#if USBD_MSC_CLASS_ACTIVATED == 1U
  /* Initialize the storage class parameters for the device */
  storage_parameter.ux_slave_class_storage_instance_activate   = USBD_STORAGE_Activate;
  storage_parameter.ux_slave_class_storage_instance_deactivate = USBD_STORAGE_Deactivate;

  /* Store the number of LUN in this device storage instance */
  storage_parameter.ux_slave_class_storage_parameter_number_lun = STORAGE_NUMBER_LUN;

  /* Initialize the storage class parameters for reading/writing to the Flash Disk */
  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_last_lba = USBD_STORAGE_GetMediaLastLba();

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_block_length = USBD_STORAGE_GetMediaBlocklength();

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_type = 0;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_removable_flag = STORAGE_REMOVABLE_FLAG;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_read_only_flag = STORAGE_READ_ONLY;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_read = USBD_STORAGE_Read;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_write = USBD_STORAGE_Write;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_flush = USBD_STORAGE_Flush;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_status = USBD_STORAGE_Status;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].
    ux_slave_class_storage_media_notification = USBD_STORAGE_Notification;

  /* USER CODE BEGIN STORAGE_PARAMETER */

  /* USER CODE END STORAGE_PARAMETER */

  /* Get storage configuration number */
  storage_configuration_number = USBD_Get_Configuration_Number(CLASS_TYPE_MSC, 0);

  /* Find storage interface number */
  storage_interface_number = USBD_Get_Interface_Number(CLASS_TYPE_MSC, 0);

  /* Initialize the device storage class */
  if (ux_device_stack_class_register(_ux_system_slave_class_storage_name,
                                     ux_device_class_storage_entry,
                                     storage_configuration_number,
                                     storage_interface_number,
                                     &storage_parameter) != UX_SUCCESS)
  {
    /* USER CODE BEGIN USBX_DEVICE_STORAGE_REGISTER_ERROR */
    LED_FatalStageCode(5U, 1U);
    /* USER CODE END USBX_DEVICE_STORAGE_REGISTER_ERROR */
  }
#endif /* USBD_MSC_CLASS_ACTIVATED */

  /* Allocate the stack for device application main thread */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer, UX_DEVICE_APP_THREAD_STACK_SIZE,
                       TX_NO_WAIT) != TX_SUCCESS)
  {
    /* USER CODE BEGIN MAIN_THREAD_ALLOCATE_STACK_ERROR */
    LED_FatalStageCode(6U, 1U);
    /* USER CODE END MAIN_THREAD_ALLOCATE_STACK_ERROR */
  }

  /* Create the device application main thread */
  if (tx_thread_create(&ux_device_app_thread, UX_DEVICE_APP_THREAD_NAME, app_ux_device_thread_entry,
                       0, pointer, UX_DEVICE_APP_THREAD_STACK_SIZE, UX_DEVICE_APP_THREAD_PRIO,
                       UX_DEVICE_APP_THREAD_PREEMPTION_THRESHOLD, UX_DEVICE_APP_THREAD_TIME_SLICE,
                       UX_DEVICE_APP_THREAD_START_OPTION) != TX_SUCCESS)
  {
    /* USER CODE BEGIN MAIN_THREAD_CREATE_ERROR */
    LED_FatalStageCode(7U, 1U);
    /* USER CODE END MAIN_THREAD_CREATE_ERROR */
  }

  /* USER CODE BEGIN MX_USBX_Device_Init1 */
  /* Logger runs in its own low-priority thread; CDC activate will only enable it when DTR is set. */
  /* Logger_Init() is called in App_ThreadX_Init */
  /* USER CODE END MX_USBX_Device_Init1 */

  return ret;
}

/**
  * @brief  Function implementing app_ux_device_thread_entry.
  * @param  thread_input: User thread input parameter.
  * @retval none
  */
static VOID app_ux_device_thread_entry(ULONG thread_input)
{
  /* USER CODE BEGIN app_ux_device_thread_entry */
  TX_PARAMETER_NOT_USED(thread_input);

  /* Defensive: make sure we are not running with IRQs masked.
   * If ThreadX low-level init ever leaves PRIMASK/BASEPRI asserted,
   * USB will appear totally dead (no enumeration).
   */
  __enable_irq();
  __set_BASEPRI(0U);

  /* Initialize the USB device controller HAL driver */
  MX_USB_PCD_Init();

  /* Configure PMA (Packet Memory Area) for endpoints.
   * Total 8 endpoints (0-7). BDT (Buffer Descriptor Table) takes 8 * 8 = 64 bytes (0x00 - 0x3F).
   * Buffers must start after 0x40 to avoid overwriting BDT.
   *
   * Layout:
   * EP0 OUT: 0x40 (64 bytes)
   * EP0 IN : 0x80 (64 bytes)
   * MSC OUT: 0xC0 (64 bytes)
   * MSC IN : 0x100 (64 bytes)
   * CDC OUT: 0x140 (64 bytes)
   * CDC IN : 0x180 (64 bytes)
   * CDC CMD: 0x1C0 (64 bytes)
   */
  
  /* EP0 OUT */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x00, PCD_SNG_BUF, 0x40);
  /* EP0 IN */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x80, PCD_SNG_BUF, 0x80);

  /* MSC EP OUT (0x01) */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x01, PCD_SNG_BUF, 0xC0);
  /* MSC EP IN (0x81) */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x81, PCD_SNG_BUF, 0x100);

  /* CDC DATA EP OUT (0x03) */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x03, PCD_SNG_BUF, 0x140);
  /* CDC DATA EP IN (0x83) */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x83, PCD_SNG_BUF, 0x180);
  /* CDC CMD EP IN (0x82) */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x82, PCD_SNG_BUF, 0x1C0);

  /* Initialize the device controller driver */
  _ux_dcd_stm32_initialize((ULONG)USB_DRD_FS, (ULONG)&hpcd_USB_DRD_FS);

  /* Start the USB device */
  HAL_PCD_Start(&hpcd_USB_DRD_FS);

  /* Nothing else to do in this thread for RTOS USBX.
   * USBX classes run their own threads.
   */
  for (;;)
  {
    tx_thread_sleep((ULONG)TX_TIMER_TICKS_PER_SECOND);
  }
  /* USER CODE END app_ux_device_thread_entry */
}

/* USER CODE BEGIN 1 */

#if defined(USBX_STANDALONE_BRINGUP)
ULONG usbx_standalone_time_get(VOID)
{
  return (ULONG)HAL_GetTick();
}

ALIGN_TYPE usbx_standalone_irq_disable(VOID)
{
  ALIGN_TYPE primask = (ALIGN_TYPE)__get_PRIMASK();
  __disable_irq();
  return primask;
}

VOID usbx_standalone_irq_restore(ALIGN_TYPE primask)
{
  __set_PRIMASK((uint32_t)primask);
}
#endif /* USBX_STANDALONE_BRINGUP */

UINT MX_USBX_Device_Standalone_Init(VOID)
{
#if defined(USBX_STANDALONE_BRINGUP)
  UINT ret;
  UCHAR *device_framework_high_speed;
  UCHAR *device_framework_full_speed;
  ULONG device_framework_hs_length;
  ULONG device_framework_fs_length;
  ULONG string_framework_length;
  ULONG language_id_framework_length;
  UCHAR *string_framework;
  UCHAR *language_id_framework;

  /* USBX memory pool for standalone mode. */
  /* Ensure alignment for USBX structures */
  static UCHAR ux_standalone_memory[USBX_DEVICE_MEMORY_STACK_SIZE] __attribute__((aligned(32)));

  ret = ux_system_initialize(ux_standalone_memory,
                             (ULONG)sizeof(ux_standalone_memory),
                             UX_NULL,
                             0U);
  if (ret != UX_SUCCESS)
  {
    return ret;
  }

  device_framework_high_speed = USBD_Get_Device_Framework_Speed(USBD_HIGH_SPEED,
                                                                &device_framework_hs_length);

  device_framework_full_speed = USBD_Get_Device_Framework_Speed(USBD_FULL_SPEED,
                                                                &device_framework_fs_length);

  string_framework = USBD_Get_String_Framework(&string_framework_length);
  language_id_framework = USBD_Get_Language_Id_Framework(&language_id_framework_length);

  ret = ux_device_stack_initialize(device_framework_high_speed,
                                   device_framework_hs_length,
                                   device_framework_full_speed,
                                   device_framework_fs_length,
                                   string_framework,
                                   string_framework_length,
                                   language_id_framework,
                                   language_id_framework_length,
                                   UX_NULL);
  if (ret != UX_SUCCESS)
  {
    return ret;
  }

  /* CDC ACM parameters */
  cdc_acm_parameter.ux_slave_class_cdc_acm_instance_activate   = USBD_CDC_ACM_Activate;
  cdc_acm_parameter.ux_slave_class_cdc_acm_instance_deactivate = USBD_CDC_ACM_Deactivate;
  cdc_acm_parameter.ux_slave_class_cdc_acm_parameter_change    = USBD_CDC_ACM_ParameterChange;

  cdc_acm_configuration_number = USBD_Get_Configuration_Number(CLASS_TYPE_CDC_ACM, 0);
  cdc_acm_interface_number = USBD_Get_Interface_Number(CLASS_TYPE_CDC_ACM, 0);

  ret = ux_device_stack_class_register(_ux_system_slave_class_cdc_acm_name,
                                       ux_device_class_cdc_acm_entry,
                                       cdc_acm_configuration_number,
                                       cdc_acm_interface_number,
                                       &cdc_acm_parameter);
  if (ret != UX_SUCCESS)
  {
    return ret;
  }

  /* MSC parameters */
  storage_parameter.ux_slave_class_storage_instance_activate   = USBD_STORAGE_Activate;
  storage_parameter.ux_slave_class_storage_instance_deactivate = USBD_STORAGE_Deactivate;
  storage_parameter.ux_slave_class_storage_parameter_number_lun = STORAGE_NUMBER_LUN;

  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_last_lba =
    USBD_STORAGE_GetMediaLastLba();
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_block_length =
    USBD_STORAGE_GetMediaBlocklength();
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_type = 0;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_removable_flag = STORAGE_REMOVABLE_FLAG;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read_only_flag = STORAGE_READ_ONLY;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_read = USBD_STORAGE_Read;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_write = USBD_STORAGE_Write;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_flush = USBD_STORAGE_Flush;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_status = USBD_STORAGE_Status;
  storage_parameter.ux_slave_class_storage_parameter_lun[0].ux_slave_class_storage_media_notification = USBD_STORAGE_Notification;

  storage_configuration_number = USBD_Get_Configuration_Number(CLASS_TYPE_MSC, 0);
  storage_interface_number = USBD_Get_Interface_Number(CLASS_TYPE_MSC, 0);

  ret = ux_device_stack_class_register(_ux_system_slave_class_storage_name,
                                       ux_device_class_storage_entry,
                                       storage_configuration_number,
                                       storage_interface_number,
                                       &storage_parameter);
  if (ret != UX_SUCCESS)
  {
    return ret;
  }

  /* Logger is ThreadX-based; keep it inert in standalone bring-up. */
  Logger_Init();

  return UX_SUCCESS;
#else
  return UX_ERROR;
#endif
}

/* USER CODE END 1 */

/**
  * @brief  USBD_CDC_ACM_Activate
  *         This function is called when insertion of a CDC ACM device.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
/* Implemented in ux_device_cdc_acm.c to handle DTR logic */
/*
VOID USBD_CDC_ACM_Activate(VOID *cdc_acm_instance)
{
  Logger_SetCdcInstance((UX_SLAVE_CLASS_CDC_ACM*)cdc_acm_instance);
  return;
}

VOID USBD_CDC_ACM_Deactivate(VOID *cdc_acm_instance)
{
  UX_PARAMETER_NOT_USED(cdc_acm_instance);
  Logger_SetCdcInstance(UX_NULL);
  return;
}

VOID USBD_CDC_ACM_ParameterChange(VOID *cdc_acm_instance)
{
  UX_PARAMETER_NOT_USED(cdc_acm_instance);
  return;
}
*/
