/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_threadx.c
  * @author  MCD Application Team
  * @brief   ThreadX applicative file
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
#include "app_threadx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "led_status.h"
#include "logger.h"
#include "button_handler.h"
#include "fs_reader.h"
#include "jpeg_processor.h"
#include "ux_device_class_cdc_acm.h"
#include <stdio.h>
#include <string.h>

#include "main.h"

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
/* USER CODE BEGIN PV */

static TX_THREAD logger_flush_thread;
static UCHAR logger_flush_thread_stack[1024];

extern UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance_ptr;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

static VOID logger_flush_thread_entry(ULONG thread_input);

/* USER CODE END PFP */

/**
  * @brief  Application ThreadX Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT App_ThreadX_Init(VOID *memory_ptr)
{
  UINT ret = TX_SUCCESS;
  (void)memory_ptr;
  /* USER CODE END App_ThreadX_MEM_POOL */
  /* USER CODE BEGIN App_ThreadX_Init */
  
  /* NOTE: This function runs BEFORE the ThreadX scheduler starts.
   * Do NOT use tx_thread_sleep() or other blocking calls here.
   * Only create threads/queues - they will start after tx_kernel_enter().
   */
  
  /* Phase 1: Initialize Logger (for buffered log output) */
  Logger_Init();
  
  /* Create thread to flush buffered logs after boot */
  tx_thread_create(&logger_flush_thread, "LogFlush",
                   logger_flush_thread_entry, 0,
                   logger_flush_thread_stack, sizeof(logger_flush_thread_stack),
                   25, 25, TX_NO_TIME_SLICE, TX_AUTO_START);

  /* Phase 2: Initialize JPEG processor FIRST (button handler depends on it) */
  JPEG_Processor_Status_t jpeg_status = JPEG_Processor_Init();
  if (jpeg_status != JPEG_PROC_OK)
  {
    LOG_ERROR_TAG("BOOT", "JPEG processor init failed: %d", (int)jpeg_status);
  }
  else
  {
    LOG_INFO_TAG("BOOT", "JPEG processor ready");
  }

  /* Phase 3: Initialize button handler (uses JPEG processor) */
  ButtonHandler_Init(UX_NULL);

  /* Phase 4: Initialize filesystem reader (requires SD card) */
  FS_Reader_Init(UX_NULL);

  /* USER CODE END App_ThreadX_Init */

  return ret;
}

  /**
  * @brief  Function that implements the kernel's initialization.
  * @param  None
  * @retval None
  */
void MX_ThreadX_Init(void)
{
  /* USER CODE BEGIN Before_Kernel_Start */

  /* Short “about to enter ThreadX” marker.
   * Leave LED OFF afterwards so the USBX device thread can own the LED state.
   */
  LED_On();
  HAL_Delay(50U);
  LED_Off();
  HAL_Delay(50U);
  LED_On();
  HAL_Delay(50U);
  LED_Off();

  /* USER CODE END Before_Kernel_Start */

  tx_kernel_enter();

  /* USER CODE BEGIN Kernel_Start_Error */

  /* tx_kernel_enter() should never return. */
  LED_FatalStageCode(30U, 1U);

  /* USER CODE END Kernel_Start_Error */
}

/* USER CODE BEGIN 1 */

/**
  * @brief  Logger flush thread - waits 5 seconds then logs init message to flush buffer
  */
static VOID logger_flush_thread_entry(ULONG thread_input)
{
  TX_PARAMETER_NOT_USED(thread_input);

  /* Wait 5 seconds for terminal to connect */
  tx_thread_sleep(5U * TX_TIMER_TICKS_PER_SECOND);
  
  /* Log init message - this will flush all buffered boot logs */
  LOG_INFO("Logger initialized");
  
  /* Thread exits - no longer needed */
}

/* USER CODE END 1 */
