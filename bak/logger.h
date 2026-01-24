/**
  ******************************************************************************
  * @file    logger.h
  * @brief   Logger header file for USB CDC output
  ******************************************************************************
  */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdint.h>

/* Log level definitions */
#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

/* Log level configuration - default to DEBUG for development */
#ifndef LOG_LEVEL
    #define LOG_LEVEL LOG_LEVEL_DEBUG
#endif

#define LOG_MAX_LENGTH 256

/* Color codes for terminal output */
#define LOG_COLOR_RESET  "\033[0m"
#define LOG_COLOR_DEBUG  "\033[0;36m"  /* Cyan */
#define LOG_COLOR_INFO   "\033[0;32m"  /* Green */
#define LOG_COLOR_WARN   "\033[0;33m"  /* Yellow */
#define LOG_COLOR_ERROR  "\033[0;31m"  /* Red */

/* Simple logging macros */
#define LOG_DEBUG(message) Logger_Log(LOG_LEVEL_DEBUG, message)
#define LOG_INFO(message)  Logger_Log(LOG_LEVEL_INFO, message)
#define LOG_WARN(message)  Logger_Log(LOG_LEVEL_WARN, message)
#define LOG_ERROR(message) Logger_Log(LOG_LEVEL_ERROR, message)

/* Tagged logging macros with variable arguments */
#define LOG_DEBUG_TAG(tag, format, ...) do { \
    if (LOG_LEVEL >= LOG_LEVEL_DEBUG) { \
        char buf[LOG_MAX_LENGTH]; \
        snprintf(buf, sizeof(buf), "[%s] " format, tag, ##__VA_ARGS__); \
        Logger_Log(LOG_LEVEL_DEBUG, buf); \
    } \
} while(0)

#define LOG_INFO_TAG(tag, format, ...) do { \
    if (LOG_LEVEL >= LOG_LEVEL_INFO) { \
        char buf[LOG_MAX_LENGTH]; \
        snprintf(buf, sizeof(buf), "[%s] " format, tag, ##__VA_ARGS__); \
        Logger_Log(LOG_LEVEL_INFO, buf); \
    } \
} while(0)

#define LOG_WARN_TAG(tag, format, ...) do { \
    if (LOG_LEVEL >= LOG_LEVEL_WARN) { \
        char buf[LOG_MAX_LENGTH]; \
        snprintf(buf, sizeof(buf), "[%s] " format, tag, ##__VA_ARGS__); \
        Logger_Log(LOG_LEVEL_WARN, buf); \
    } \
} while(0)

#define LOG_ERROR_TAG(tag, format, ...) do { \
    if (LOG_LEVEL >= LOG_LEVEL_ERROR) { \
        char buf[LOG_MAX_LENGTH]; \
        snprintf(buf, sizeof(buf), "[%s] " format, tag, ##__VA_ARGS__); \
        Logger_Log(LOG_LEVEL_ERROR, buf); \
    } \
} while(0)

/* Function tracing macros */
#define LOG_FUNCTION_ENTRY_TAG(tag) LOG_DEBUG_TAG(tag, "Entering %s", __func__)
#define LOG_FUNCTION_EXIT_TAG(tag)  LOG_DEBUG_TAG(tag, "Exiting %s", __func__)

/* Forward declaration for USBX CDC ACM type */
struct UX_SLAVE_CLASS_CDC_ACM_STRUCT;

/* Public function declarations */
void Logger_Init(void);
void Logger_Log(int level, const char *message);
void Logger_Write(const char *data, uint32_t length);
int  Logger_IsReady(void);
void Logger_SetCdcInstance(struct UX_SLAVE_CLASS_CDC_ACM_STRUCT *instance);

#endif /* LOGGER_H */
