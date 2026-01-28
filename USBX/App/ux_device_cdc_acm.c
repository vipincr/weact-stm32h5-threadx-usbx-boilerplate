/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ux_device_cdc_acm.c
  * @author  MCD Application Team
  * @brief   USBX Device CDC ACM applicative file
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
#include "ux_device_cdc_acm.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "logger.h"
#include "led_status.h"
#include "ux_device_class_cdc_acm.h"
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
static UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance_ptr = UX_NULL;
static ULONG cdc_last_line_state = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static void cdc_update_led_from_line_state(UX_SLAVE_CLASS_CDC_ACM *instance);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */


static void cdc_update_led_from_line_state(UX_SLAVE_CLASS_CDC_ACM *instance)
{
  ULONG line_state = 0U;

  if (instance == UX_NULL)
  {
    cdc_last_line_state = 0U;
    Logger_SetCdcInstance(UX_NULL);
    return;
  }

  (void)ux_device_class_cdc_acm_ioctl(instance,
                                      UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_STATE,
                                      &line_state);

  /* Connected only when host opens CDC (DTR asserted). */
  if ((line_state & UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_DTR) != 0U)
  {
    if ((cdc_last_line_state & UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_DTR) == 0U)
    {
      Logger_SetCdcInstance(cdc_acm_instance_ptr);
      LOG_INFO_TAG("CDC", "Connected");
    }
  }
  else
  {
    if ((cdc_last_line_state & UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_DTR) != 0U)
    {
      LOG_INFO_TAG("CDC", "Disconnected");
      Logger_SetCdcInstance(UX_NULL);
    }
  }

  cdc_last_line_state = line_state;
}
/* USER CODE END 0 */

/**
  * @brief  USBD_CDC_ACM_Activate
  *         This function is called when insertion of a CDC ACM device.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
VOID USBD_CDC_ACM_Activate(VOID *cdc_acm_instance)
{
  /* USER CODE BEGIN USBD_CDC_ACM_Activate */
  /* Save the CDC instance */
  cdc_acm_instance_ptr = (UX_SLAVE_CLASS_CDC_ACM *)cdc_acm_instance;

  /* Set the default line coding parameters (115200, 8N1) */
  UX_SLAVE_CLASS_CDC_ACM_LINE_CODING_PARAMETER line_coding;
  line_coding.ux_slave_class_cdc_acm_parameter_baudrate = 115200;
  line_coding.ux_slave_class_cdc_acm_parameter_stop_bit = 0; /* 1 stop bit */
  line_coding.ux_slave_class_cdc_acm_parameter_parity = 0;   /* No parity */
  line_coding.ux_slave_class_cdc_acm_parameter_data_bit = 8; /* 8 data bits */

  ux_device_class_cdc_acm_ioctl(cdc_acm_instance_ptr,
                                UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_LINE_CODING,
                                &line_coding);

#ifndef UX_STANDALONE
  /* Set write timeout to avoid indefinte blocking if host stops reading */
  /* Timeout: 100ms (assuming 1ms tick) */
  ULONG write_timeout = 100;
  ux_device_class_cdc_acm_ioctl(cdc_acm_instance_ptr,
                                UX_SLAVE_CLASS_CDC_ACM_IOCTL_SET_WRITE_TIMEOUT,
                                &write_timeout);
#endif

  /* Initialize logger with CDC instance */
  Logger_SetCdcInstance(cdc_acm_instance_ptr);


    /* At activation time, the host may not have opened the port yet. */
    cdc_update_led_from_line_state(cdc_acm_instance_ptr);
  /* USER CODE END USBD_CDC_ACM_Activate */

  return;
}

/**
  * @brief  USBD_CDC_ACM_Deactivate
  *         This function is called when extraction of a CDC ACM device.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
VOID USBD_CDC_ACM_Deactivate(VOID *cdc_acm_instance)
{
  /* USER CODE BEGIN USBD_CDC_ACM_Deactivate */
  UX_PARAMETER_NOT_USED(cdc_acm_instance);

  /* Clear logger CDC instance before resetting */
  Logger_SetCdcInstance(UX_NULL);

  /* Reset the cdc acm instance */
  cdc_acm_instance_ptr = UX_NULL;

    /* Turn LED off when CDC is gone */
    cdc_update_led_from_line_state(UX_NULL);
  /* USER CODE END USBD_CDC_ACM_Deactivate */

  return;
}

/**
  * @brief  USBD_CDC_ACM_ParameterChange
  *         This function is invoked when line coding parameters change.
  * @param  cdc_acm_instance: Pointer to the cdc acm class instance.
  * @retval none
  */
VOID USBD_CDC_ACM_ParameterChange(VOID *cdc_acm_instance)
{
  /* USER CODE BEGIN USBD_CDC_ACM_ParameterChange */
  UX_PARAMETER_NOT_USED(cdc_acm_instance);

    /* Host may change line coding and/or control line state (DTR/RTS). */
    cdc_update_led_from_line_state(cdc_acm_instance_ptr);
  /* USER CODE END USBD_CDC_ACM_ParameterChange */

  return;
}

/* USER CODE BEGIN 1 */
/**
  * @brief  USBD_CDC_ACM_Write
  *         Write data to the CDC ACM interface
  * @param  buffer: Pointer to data buffer
  * @param  length: Length of data to write
  * @param  actual_length: Pointer to store actual bytes written
  * @retval status
  */
UINT USBD_CDC_ACM_Write(UCHAR *buffer, ULONG length, ULONG *actual_length)
{
  UINT status = UX_ERROR;

  if (cdc_acm_instance_ptr != UX_NULL)
  {
    status = ux_device_class_cdc_acm_write(cdc_acm_instance_ptr, buffer, length, actual_length);
  }

  return status;
}

/**
  * @brief  USBD_CDC_ACM_Read
  *         Read data from the CDC ACM interface
  * @param  buffer: Pointer to data buffer
  * @param  length: Length of buffer
  * @param  actual_length: Pointer to store actual bytes read
  * @retval status
  */
UINT USBD_CDC_ACM_Read(UCHAR *buffer, ULONG length, ULONG *actual_length)
{
  UINT status = UX_ERROR;

  if (cdc_acm_instance_ptr != UX_NULL)
  {
    status = ux_device_class_cdc_acm_read(cdc_acm_instance_ptr, buffer, length, actual_length);
  }

  return status;
}

/**
  * @brief  USBD_CDC_ACM_PollLineState
  *         Periodically poll and update line state (DTR/RTS).
  *         Call this from main loop to detect DTR changes that
  *         might not trigger ParameterChange callback.
  * @retval none
  */
VOID USBD_CDC_ACM_PollLineState(VOID)
{
  if (cdc_acm_instance_ptr != UX_NULL)
  {
    cdc_update_led_from_line_state(cdc_acm_instance_ptr);
  }
}
/* USER CODE END 1 */
