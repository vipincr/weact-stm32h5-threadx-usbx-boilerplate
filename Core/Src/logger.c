/**
  * @file    logger.c
  * @brief   Simple logger with ring buffer - flushes to CDC when terminal ready
  */

#include "logger.h"
#include <string.h>
#include <stdio.h>

extern UX_SLAVE_CLASS_CDC_ACM *cdc_acm_instance_ptr;

/* Simple ring buffer - stores raw bytes */
#define RING_SIZE 2048
static char ring_buf[RING_SIZE];
static volatile uint32_t ring_head = 0U;  /* Write position */
static volatile uint32_t ring_tail = 0U;  /* Read position */

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
  
  /* Only flush if terminal is actually connected and ready */
  if (!terminal_ready())
    return;
  
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
      ux_device_class_cdc_acm_write(cdc_acm_instance_ptr, (UCHAR*)chunk, chunk_len, &actual);
    }
  }
}

void Logger_Init(void) {}

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
  char buf[160];
  int len;
  const char *prefix;
  
  if (message == NULL) return;
  if (level > LOG_LEVEL) return;
  
  switch (level) {
    case LOG_LEVEL_ERROR: prefix = "\033[31m[ERROR] "; break;
    case LOG_LEVEL_WARN:  prefix = "\033[33m[WARN]  "; break;
    case LOG_LEVEL_INFO:  prefix = "\033[32m[INFO]  "; break;
    case LOG_LEVEL_DEBUG: prefix = "\033[36m[DEBUG] "; break;
    default: prefix = ""; break;
  }
  
  len = snprintf(buf, sizeof(buf), "%s%s\033[0m\r\n", prefix, message);
  if (len > 0)
  {
    ring_write(buf, (uint32_t)len);
    ring_flush();
  }
}

int _write(int file, char *ptr, int len)
{
  (void)file;
  if (len > 0)
  {
    ring_write(ptr, (uint32_t)len);
    ring_flush();
  }
  return len;
}
