/**
  ******************************************************************************
  * @file    logger.c
  * @brief   Logger implementation for USB CDC output
  ******************************************************************************
  */

/* USER CODE BEGIN Header */

/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "logger.h"
#include <string.h>
#include <stdio.h>

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "ux_api.h"
#include "ux_device_class_cdc_acm.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define LOGGER_TAG "LOGGER"
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance = UX_NULL;
static UINT logger_ready = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static const char *Logger_GetLevelString(int level);
static const char *Logger_GetLevelColor(int level);
/* USER CODE END PFP */

/* Public functions ----------------------------------------------------------*/

/**
  * @brief  Initialize the logger module
  * @retval None
  */
void Logger_Init(void)
{
  /* Logger will be ready when CDC ACM is activated */
  logger_ready = 0U;
}

/**
  * @brief  Set the CDC ACM instance for logging output
  * @param  instance: Pointer to CDC ACM instance
  * @retval None
  */
void Logger_SetCdcInstance(UX_SLAVE_CLASS_CDC_ACM *instance)
{
  cdc_acm_instance = instance;
  if (instance != UX_NULL)
  {
    logger_ready = 1U;
  }
  else
  {
    logger_ready = 0U;
  }
}

/**
  * @brief  Check if logger is ready for output
  * @retval 1 if ready, 0 otherwise
  */
int Logger_IsReady(void)
{
  return (logger_ready != 0U) ? 1 : 0;
}

/**
  * @brief  Write raw data to the logger output
  * @param  data: Data buffer to write
  * @param  length: Length of data
  * @retval None
  */
void Logger_Write(const char *data, uint32_t length)
{
  ULONG actual_length;

  if ((cdc_acm_instance == UX_NULL) || (logger_ready == 0U))
  {
    return;
  }

  if ((data == UX_NULL) || (length == 0U))
  {
    return;
  }

  /* Write to USB CDC */
#if defined(UX_DEVICE_STANDALONE)
  (void)ux_device_class_cdc_acm_write_run(cdc_acm_instance,
                                          (UCHAR *)data,
                                          length,
                                          &actual_length);
#else
  (void)ux_device_class_cdc_acm_write(cdc_acm_instance,
                                       (UCHAR *)data,
                                       length,
                                       &actual_length);
#endif
}

/**
  * @brief  Log a message with specified level
  * @param  level: Log level (LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, etc.)
  * @param  message: Message string to log
  * @retval None
  */
void Logger_Log(int level, const char *message)
{
  char buffer[LOG_MAX_LENGTH + 32];
  int len;

  if (level > LOG_LEVEL)
  {
    return;
  }

  if ((cdc_acm_instance == UX_NULL) || (logger_ready == 0U))
  {
    return;
  }

  if (message == UX_NULL)
  {
    return;
  }

  /* Format: [COLOR][LEVEL][RESET] message\r\n */
  len = snprintf(buffer, sizeof(buffer), "%s[%s]%s %s\r\n",
                 Logger_GetLevelColor(level),
                 Logger_GetLevelString(level),
                 LOG_COLOR_RESET,
                 message);

  if (len > 0)
  {
    Logger_Write(buffer, (uint32_t)len);
  }
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Get level string representation
  * @param  level: Log level
  * @retval Level string
  */
static const char *Logger_GetLevelString(int level)
{
  switch (level)
  {
    case LOG_LEVEL_DEBUG:
      return "DEBUG";
    case LOG_LEVEL_INFO:
      return "INFO ";
    case LOG_LEVEL_WARN:
      return "WARN ";
    case LOG_LEVEL_ERROR:
      return "ERROR";
    default:
      return "?????";
  }
}

/**
  * @brief  Get level color code
  * @param  level: Log level
  * @retval Color code string
  */
static const char *Logger_GetLevelColor(int level)
{
  switch (level)
  {
    case LOG_LEVEL_DEBUG:
      return LOG_COLOR_DEBUG;
    case LOG_LEVEL_INFO:
      return LOG_COLOR_INFO;
    case LOG_LEVEL_WARN:
      return LOG_COLOR_WARN;
    case LOG_LEVEL_ERROR:
      return LOG_COLOR_ERROR;
    default:
      return LOG_COLOR_RESET;
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
