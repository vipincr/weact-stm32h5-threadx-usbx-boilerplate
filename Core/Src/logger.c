/**
  ******************************************************************************
  * @file    logger.c
  * @brief   Logger implementation for USB CDC output (ThreadX/USBX)
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "logger.h"
#include <string.h>
#include <stdio.h>
#include "main.h" /* For CMSIS intrinsics */

/* Private define ------------------------------------------------------------*/
#define LOGGER_THREAD_STACK_SIZE     (2048U)
#define LOGGER_THREAD_PRIORITY       (20U) /* Medium priority */
#define LOGGER_BUFFER_SIZE           (4096U) /* 4KB Ring Buffer */
#define LOGGER_BUFFER_MASK           (LOGGER_BUFFER_SIZE - 1U)

/* Ensure power of 2 for mask to work */
#if (LOGGER_BUFFER_SIZE & LOGGER_BUFFER_MASK) != 0
#error "LOGGER_BUFFER_SIZE must be a power of 2"
#endif

/* Private variables ---------------------------------------------------------*/
static char logger_buffer[LOGGER_BUFFER_SIZE] __attribute__((aligned(32)));
static volatile uint32_t logger_head = 0U;
static volatile uint32_t logger_tail = 0U;
static volatile uint32_t logger_dropped_bytes = 0U;

static TX_THREAD logger_thread;
static ULONG logger_thread_stack[LOGGER_THREAD_STACK_SIZE / sizeof(ULONG)];
static TX_SEMAPHORE logger_sem;

/* USB CDC Instance and state */
static UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance = UX_NULL;
static volatile uint32_t logger_dtr_ready = 0U;
static volatile uint32_t logger_initialized = 0U;

#if defined(UX_STANDALONE)
/* Standalone specific state */
/* Intermediate buffer for USB transfer to ensure alignment and data stability */
static uint8_t logger_tx_buffer[64] __attribute__((aligned(32))); 
static ULONG logger_transfer_len = 0;
static ULONG logger_transfer_actual_len = 0; /* Must be static/persistent for write_run */
static ULONG logger_transfer_active = 0;
static const char *logger_transfer_ptr = (const char *)logger_tx_buffer; /* Unused but kept for structure */
#endif

/* Private function prototypes -----------------------------------------------*/
static VOID logger_thread_entry(ULONG thread_input);
static void Logger_WriteRaw(const char *data, uint32_t length);

/* Public functions ----------------------------------------------------------*/

/**
  * @brief  Initialize the logger module (ThreadX objects)
  * @retval None
  */
void Logger_Init(void)
{
  if (logger_initialized) return;

  /* Initialize Buffer */
  /* Do not clear buffer if it already contains data (from boot logs) */
  /* logger_head = 0U; */
  /* logger_tail = 0U; */
  logger_dropped_bytes = 0U;
  logger_dtr_ready = 0U;
  cdc_acm_instance = UX_NULL;

#if !defined(UX_STANDALONE)
  /* Create Semaphore */
  if (tx_semaphore_create(&logger_sem, "Logger Sem", 0) != TX_SUCCESS)
  {
    return;
  }

  /* Create Thread */
  if (tx_thread_create(&logger_thread,
                       "Logger Thread",
                       logger_thread_entry,
                       0,
                       logger_thread_stack,
                       LOGGER_THREAD_STACK_SIZE,
                       LOGGER_THREAD_PRIORITY,
                       LOGGER_THREAD_PRIORITY,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS)
  {
      return;
  }
#endif
  
  logger_initialized = 1U;
}

/**
  * @brief  Write a log message
  * @param  level: Log level
  * @param  message: Message string
  * @retval None
  */
void Logger_Log(int level, const char *message)
{
  char buffer[LOG_MAX_LENGTH + 32];
  int len;

  if (level > LOG_LEVEL) return;
  if (message == UX_NULL) return;

  /* Format: [COLOR][Message]\r\n (Using the macros from header for color) */
  /* Replicate the format from the attached logger.c: [LEVEL] message */
  
  const char* level_color = LOG_COLOR_RESET;
  switch(level) {
      case LOG_LEVEL_DEBUG: level_color = LOG_COLOR_DEBUG; break;
      case LOG_LEVEL_INFO:  level_color = LOG_COLOR_INFO; break;
      case LOG_LEVEL_WARN:  level_color = LOG_COLOR_WARN; break;
      case LOG_LEVEL_ERROR: level_color = LOG_COLOR_ERROR; break;
  }

  len = snprintf(buffer, sizeof(buffer), "%s %s%s\r\n", 
                 level_color, 
                 message, 
                 LOG_COLOR_RESET);

  if (len > 0)
  {
    Logger_WriteRaw(buffer, (uint32_t)len);
  }
}

/**
  * @brief  Set CDC Instance (called from USBX app)
  */
void Logger_SetCdcInstance(UX_SLAVE_CLASS_CDC_ACM *instance)
{
  cdc_acm_instance = instance;
  if (instance != UX_NULL)
  {
    logger_dtr_ready = 1U;
    /* Signal thread to flush any pending data */
    if (logger_initialized)
    {
        tx_semaphore_put(&logger_sem);
    }
  }
  else
  {
    logger_dtr_ready = 0U;
  }
}

int Logger_IsReady(void)
{
    return (logger_dtr_ready && logger_initialized);
}

/* Private functions ---------------------------------------------------------*/

static void Logger_WriteRaw(const char *data, uint32_t length)
{
  uint32_t i;
  uint32_t space_avail;
  UINT old_primask;

  if (length == 0) return;

  /* Critical Section for Buffer Access */
  old_primask = __get_PRIMASK();
  __disable_irq();

  space_avail = (LOGGER_BUFFER_SIZE - 1U) - ((logger_head - logger_tail) & LOGGER_BUFFER_MASK);

  if (length > space_avail)
  {
    logger_dropped_bytes += length;
  }
  else
  {
    for (i = 0; i < length; i++)
    {
      logger_buffer[logger_head] = data[i];
      logger_head = (logger_head + 1U) & LOGGER_BUFFER_MASK;
    }
  }

  __set_PRIMASK(old_primask);

  /* Signal Thread (only if initialized and not standalone) */
#if !defined(UX_STANDALONE)
  if (logger_initialized)
  {
    tx_semaphore_put(&logger_sem);
  }
#else
  (void)logger_sem;
#endif
}

#if defined(UX_STANDALONE)
void Logger_Run(void)
{
    uint32_t current_head;
    UINT status;

    if (cdc_acm_instance == UX_NULL)
    {
        /* Reset transfer state if disconnected */
        logger_transfer_active = 0;
        return;
    }

    /* State Machine: If no transfer active, pick new chunk */
    if (logger_transfer_active == 0)
    {
        current_head = logger_head;
        if (logger_tail == current_head) return; /* Empty */

        /* Calculate contiguous chunk (max 64) */
        if (current_head > logger_tail)
        {
            logger_transfer_len = current_head - logger_tail;
        }
        else
        {
            logger_transfer_len = LOGGER_BUFFER_SIZE - logger_tail;
        }
        if (logger_transfer_len > 64) logger_transfer_len = 64;

        logger_transfer_ptr = &logger_buffer[logger_tail];
        logger_transfer_active = 1;
        logger_transfer_actual_len = 0; /* Clear actual length */
    }

    /* Call non-blocking write run with STABLE arguments */
    /* NOTE: actual_length pointer MUST be persistent as USBX stores it in the transfer request! */
    status = ux_device_class_cdc_acm_write_run(cdc_acm_instance, 
                                            (UCHAR*)logger_transfer_ptr, 
                                            logger_transfer_len, 
                                            &logger_transfer_actual_len);

    /* Check status */
    if (status == UX_STATE_NEXT)
    {
        /* Transfer complete */
        logger_tail = (logger_tail + logger_transfer_actual_len) & LOGGER_BUFFER_MASK;
        logger_transfer_active = 0;
    }
    else if (status != UX_STATE_WAIT)
    {
         /* Error occurred (e.g. UX_STATE_EXIT or error code) */
         /* Abort this transfer and try to recover. 
            If we don't advance tail, we might retry indefinitely on bad data.
            But if we advance, we lose a chunk. 
            Resetting active flag tries again. */
         logger_transfer_active = 0;
    }
}
#else
void Logger_Run(void) {}
#endif

static VOID logger_thread_entry(ULONG thread_input)
{
  uint32_t current_head;
  uint32_t chunk_len;
  ULONG actual_length;
  UINT status;

  TX_PARAMETER_NOT_USED(thread_input);

  for (;;)
  {
    /* Wait for data */
    tx_semaphore_get(&logger_sem, TX_WAIT_FOREVER);

    /* Process Buffer */
    while (1)
    {
       /* Check connection */
       if (cdc_acm_instance == UX_NULL)
       {
           /* Not connected, stop flushing but keep data in buffer (or drop if full? Ring buffer handles full naturally) */
           break;
       }

       current_head = logger_head;
       if (logger_tail == current_head)
       {
           break; /* Empty */
       }

       /* Calculate contiguous chunk */
       if (current_head > logger_tail)
       {
           chunk_len = current_head - logger_tail;
       }
       else
       {
           chunk_len = LOGGER_BUFFER_SIZE - logger_tail;
       }

       /* Limit chunk size */
       if (chunk_len > 64) chunk_len = 64;

       /* Write to CDC */
#if defined(UX_STANDALONE)
       /* In Standalone mode, we cannot use blocking write. 
          Also, this thread doesn't run in Standalone mode (no scheduler).
          We fake success to allow compilation if this code is ever reached. */
       status = UX_SUCCESS;
       actual_length = chunk_len;
       (void)cdc_acm_instance;
#else
       status = ux_device_class_cdc_acm_write(cdc_acm_instance, 
                                              (UCHAR*)&logger_buffer[logger_tail], 
                                              chunk_len, 
                                              &actual_length);
#endif

       if (status == UX_SUCCESS)
       {
           logger_tail = (logger_tail + actual_length) & LOGGER_BUFFER_MASK;
       }
       else
       {
           /* Error (busy/disconnected), wait a bit and retry or break */
           tx_thread_sleep(1);
           if (cdc_acm_instance == UX_NULL) break;
       }
    }
  }
}

/* Override _write for printf support */
int _write(int file, char *ptr, int len)
{
    Logger_WriteRaw(ptr, (uint32_t)len);
    return len;
}
