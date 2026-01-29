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
#include "tx_api.h"
#include "ux_api.h"
#include "ux_device_class_cdc_acm.h"

/* Log level definitions */
#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

/* Log level configuration */
#ifndef LOG_LEVEL
    #define LOG_LEVEL LOG_LEVEL_DEBUG
#endif

#define LOG_MAX_LENGTH 128

/* Color codes for terminal output */
#define LOG_COLOR_RESET   "\033[0m"
#define LOG_COLOR_RED     "\033[0;31m"
#define LOG_COLOR_GREEN   "\033[0;32m"
#define LOG_COLOR_YELLOW  "\033[0;33m"
#define LOG_COLOR_CYAN    "\033[0;36m"

/* Public function declarations */
void Logger_Init(void);
void Logger_Log(int level, const char *message);
void Logger_SetCdcInstance(UX_SLAVE_CLASS_CDC_ACM *instance);
int Logger_IsReady(void);
void Logger_Run(void);

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
#define LOG_FUNCTION_EXIT_TAG(tag) LOG_DEBUG_TAG(tag, "Exiting %s", __func__)

#endif /* LOGGER_H */
