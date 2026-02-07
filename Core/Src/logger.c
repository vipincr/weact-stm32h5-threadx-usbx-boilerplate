/**
  * @file    logger.c
  * @brief   Simple logger with ring buffer - flushes to CDC when terminal ready
  *          Thread-safe: uses ThreadX mutex for synchronization
  *          Timestamps: HH:MM:SS.mmm since boot (no RTC)
  */

#include "logger.h"
#include "stm32h5xx_hal.h"
#include <string.h>
#include <stdio.h>

extern UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance_ptr;

/* Boot timestamp reference */
static uint32_t boot_tick = 0U;

/* Simple ring buffer - stores raw bytes */
#define RING_SIZE 2048
static char ring_buf[RING_SIZE];
static volatile uint32_t ring_head = 0U;  /* Write position */
static volatile uint32_t ring_tail = 0U;  /* Read position */

/* ThreadX mutex for thread-safe logging */
static TX_MUTEX logger_mutex;
static UINT logger_mutex_created = 0U;
static UINT scheduler_started = 0U;  /* Set when first thread context detected */

/* Try to acquire mutex - returns 1 if acquired, 0 if not available or not in thread context */
static int logger_lock(void)
{
  /* Don't try to lock if mutex not created yet (early boot) */
  if (logger_mutex_created == 0U)
    return 1;  /* Allow logging without lock during early boot */
  
  /* Check if we're in a thread context */
  TX_THREAD *current = tx_thread_identify();
  
  if (current == TX_NULL)
  {
    /* Not in thread context - either ISR or pre-scheduler */
    if (scheduler_started)
    {
      /* Scheduler is running, so this is an ISR - skip to avoid hang */
      return 0;
    }
    else
    {
      /* Scheduler not started yet - allow logging without mutex */
      return 1;
    }
  }
  
  /* We're in a thread - scheduler must be running */
  scheduler_started = 1U;
  
  /* Short timeout to avoid blocking USBX / MSC threads.
   * If the mutex is held by a CDC flush, we drop this message rather
   * than stalling time-critical USB operations.  50 ms is enough for
   * a normal CDC write to complete but short enough to prevent
   * cascading thread stalls. */
  if (tx_mutex_get(&logger_mutex, 5U) == TX_SUCCESS)
    return 1;
  
  return 0;
}

/* Release mutex */
static void logger_unlock(void)
{
  if (logger_mutex_created == 0U)
    return;
  
  if (tx_thread_identify() != TX_NULL)
    tx_mutex_put(&logger_mutex);
}

/* Add data to ring buffer */
static void ring_write(const char *data, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++)
  {
    uint32_t next = (ring_head + 1U) % RING_SIZE;
    if (next == ring_tail)
    {
      /* Buffer full - drop oldest */
      ring_tail = (ring_tail + 1U) % RING_SIZE;
    }
    ring_buf[ring_head] = data[i];
    ring_head = next;
  }
}

/* Check if terminal is ready (DTR set) */
static int terminal_ready(void)
{
  ULONG line_state = 0U;
  
  if (cdc_acm_instance_ptr == UX_NULL)
    return 0;
  
  ux_device_class_cdc_acm_ioctl(cdc_acm_instance_ptr,
                                UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_STATE,
                                &line_state);
  
  return (line_state & UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_DTR) ? 1 : 0;
}

/* Flush ring buffer to CDC - only if terminal is ready */
static void ring_flush(void)
{
  ULONG actual;
  char chunk[64];
  uint32_t chunk_len;

  /* Take a local copy of the CDC instance pointer to prevent TOCTOU race.
   * USBD_CDC_ACM_Deactivate (higher-priority USBX thread) can set the
   * global to NULL between our check and the write call. */
  UX_SLAVE_CLASS_CDC_ACM *cdc = cdc_acm_instance_ptr;

  if (cdc == UX_NULL)
    return;

  /* Check DTR via the local pointer */
  {
    ULONG line_state = 0U;
    ux_device_class_cdc_acm_ioctl(cdc,
                                  UX_SLAVE_CLASS_CDC_ACM_IOCTL_GET_LINE_STATE,
                                  &line_state);
    if (!(line_state & UX_SLAVE_CLASS_CDC_ACM_LINE_STATE_DTR))
      return;
  }

  while (ring_tail != ring_head)
  {
    chunk_len = 0;
    while (ring_tail != ring_head && chunk_len < sizeof(chunk))
    {
      chunk[chunk_len++] = ring_buf[ring_tail];
      ring_tail = (ring_tail + 1U) % RING_SIZE;
    }
    if (chunk_len > 0)
    {
      /* Re-verify pointer: deactivation may have occurred during a
       * previous chunk write in this same flush loop. */
      if (cdc_acm_instance_ptr == UX_NULL)
        break;

      UINT ux_status = ux_device_class_cdc_acm_write(cdc, (UCHAR*)chunk,
                                                     chunk_len, &actual);
      if (ux_status != UX_SUCCESS)
        break;  /* Endpoint stalled / host not reading - stop flushing */
    }
  }
}

void Logger_Init(void)
{
  /* Record boot time reference */
  boot_tick = HAL_GetTick();
  
  /* Create mutex for thread-safe logging */
  if (logger_mutex_created == 0U)
  {
    if (tx_mutex_create(&logger_mutex, "LoggerMutex", TX_NO_INHERIT) == TX_SUCCESS)
    {
      logger_mutex_created = 1U;
    }
  }
}

/**
  * @brief  Format elapsed time as HH:MM:SS.mmm
  */
static void format_timestamp(char *buf, size_t buf_len)
{
  uint32_t elapsed_ms = HAL_GetTick() - boot_tick;
  uint32_t ms = elapsed_ms % 1000U;
  uint32_t total_secs = elapsed_ms / 1000U;
  uint32_t secs = total_secs % 60U;
  uint32_t total_mins = total_secs / 60U;
  uint32_t mins = total_mins % 60U;
  uint32_t hours = total_mins / 60U;
  
  snprintf(buf, buf_len, "%02lu:%02lu:%02lu.%03lu",
           (unsigned long)hours, (unsigned long)mins,
           (unsigned long)secs, (unsigned long)ms);
}

void Logger_SetCdcInstance(UX_SLAVE_CLASS_CDC_ACM *instance)
{
  (void)instance;
}

int Logger_IsReady(void)
{
  return terminal_ready();
}

void Logger_Run(void) {}

void Logger_Log(int level, const char *message)
{
  char buf[192];
  char timestamp[16];
  int len;
  const char *prefix;
  
  if (message == NULL) return;
  if (level > LOG_LEVEL) return;
  
  /* Try to acquire lock - skip if in ISR or can't acquire */
  if (!logger_lock())
    return;
  
  /* Get timestamp */
  format_timestamp(timestamp, sizeof(timestamp));
  
  switch (level) {
    case LOG_LEVEL_ERROR: prefix = "\033[31m[ERROR]"; break;
    case LOG_LEVEL_WARN:  prefix = "\033[33m[WARN] "; break;
    case LOG_LEVEL_INFO:  prefix = "\033[32m[INFO] "; break;
    case LOG_LEVEL_DEBUG: prefix = "\033[36m[DEBUG]"; break;
    default: prefix = ""; break;
  }
  
  len = snprintf(buf, sizeof(buf), "[%s] %s %s\033[0m\r\n", timestamp, prefix, message);
  if (len > 0)
  {
    ring_write(buf, (uint32_t)len);
    ring_flush();
  }
  
  logger_unlock();
  
  /* Note: Removed tx_thread_relinquish() - was causing USB starvation issues.
   * USB thread runs at higher priority (10) so it will preempt anyway. */
}

int _write(int file, char *ptr, int len)
{
  (void)file;
  if (len > 0)
  {
    /* Try to acquire lock - skip output if in ISR or can't acquire */
    if (!logger_lock())
      return len;  /* Pretend we wrote it to avoid caller retrying */
    
    ring_write(ptr, (uint32_t)len);
    ring_flush();
    
    logger_unlock();
  }
  return len;
}
