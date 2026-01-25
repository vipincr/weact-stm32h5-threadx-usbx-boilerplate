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

#define LOGGER_THREAD_STACK_SIZE     (1536U)
#define LOGGER_THREAD_PRIORITY       (30U) /* lower priority than USB/RTOS work */

#define LOGGER_QUEUE_DEPTH           (16U)
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance = UX_NULL;
static UINT logger_ready = 0U;

typedef struct
{
  uint16_t len;
  char data[LOG_MAX_LENGTH + 32];
} LOGGER_MSG;

#define LOGGER_MSG_SIZE_ULONGS ((sizeof(LOGGER_MSG) + sizeof(ULONG) - 1U) / sizeof(ULONG))

static TX_THREAD logger_thread;
static TX_QUEUE logger_queue;
static ULONG *logger_thread_stack = UX_NULL;
static ULONG *logger_queue_storage = UX_NULL;
static UINT logger_thread_started = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */
static const char *Logger_GetLevelString(int level);
static const char *Logger_GetLevelColor(int level);
static void logger_thread_entry(ULONG thread_input);
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

UINT Logger_ThreadCreate(VOID *memory_ptr)
{
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL *)memory_ptr;
  UINT status;

  if (byte_pool == TX_NULL)
  {
    return TX_PTR_ERROR;
  }

  if (logger_thread_started != 0U)
  {
    return TX_SUCCESS;
  }

  /* Allocate thread stack */
  status = tx_byte_allocate(byte_pool,
                            (VOID **)&logger_thread_stack,
                            LOGGER_THREAD_STACK_SIZE,
                            TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    return status;
  }

  /* Allocate queue storage */
  status = tx_byte_allocate(byte_pool,
                            (VOID **)&logger_queue_storage,
                            (ULONG)(LOGGER_QUEUE_DEPTH * LOGGER_MSG_SIZE_ULONGS * sizeof(ULONG)),
                            TX_NO_WAIT);
  if (status != TX_SUCCESS)
  {
    return status;
  }

  status = tx_queue_create(&logger_queue,
                           (CHAR *)"LoggerQ",
                           (UINT)LOGGER_MSG_SIZE_ULONGS,
                           logger_queue_storage,
                           (ULONG)(LOGGER_QUEUE_DEPTH * LOGGER_MSG_SIZE_ULONGS * sizeof(ULONG)));
  if (status != TX_SUCCESS)
  {
    return status;
  }

  status = tx_thread_create(&logger_thread,
                            (CHAR *)"Logger",
                            logger_thread_entry,
                            0,
                            logger_thread_stack,
                            LOGGER_THREAD_STACK_SIZE,
                            LOGGER_THREAD_PRIORITY,
                            LOGGER_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
  if (status != TX_SUCCESS)
  {
    return status;
  }

  logger_thread_started = 1U;
  return TX_SUCCESS;
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
  LOGGER_MSG msg;
  UINT status;
  uint32_t copy_len;

  if ((data == UX_NULL) || (length == 0U))
  {
    return;
  }

  if (logger_thread_started == 0U)
  {
    /* Not started yet: drop (non-blocking) */
    return;
  }

  /* Truncate to message buffer */
  copy_len = length;
  if (copy_len > (uint32_t)sizeof(msg.data))
  {
    copy_len = (uint32_t)sizeof(msg.data);
  }

  msg.len = (uint16_t)copy_len;
  (void)memcpy(msg.data, data, copy_len);

  status = tx_queue_send(&logger_queue, &msg, TX_NO_WAIT);
  (void)status; /* drop on full */
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

  if (logger_thread_started == 0U)
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

static void logger_thread_entry(ULONG thread_input)
{
  ULONG actual_length;
  LOGGER_MSG msg;

  TX_PARAMETER_NOT_USED(thread_input);

  while (1)
  {
    if (tx_queue_receive(&logger_queue, &msg, TX_WAIT_FOREVER) != TX_SUCCESS)
    {
      continue;
    }

    if ((cdc_acm_instance == UX_NULL) || (logger_ready == 0U) || (msg.len == 0U))
    {
      continue;
    }

    /* Blocking write is OK here (low priority thread). */
#if defined(UX_DEVICE_STANDALONE)
    (void)ux_device_class_cdc_acm_write_run(cdc_acm_instance,
                                            (UCHAR *)msg.data,
                                            (ULONG)msg.len,
                                            &actual_length);
#else
    (void)ux_device_class_cdc_acm_write(cdc_acm_instance,
                                         (UCHAR *)msg.data,
                                         (ULONG)msg.len,
                                         &actual_length);
#endif
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
