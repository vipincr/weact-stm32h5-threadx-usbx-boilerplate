/**
  ******************************************************************************
  * @file    time_it.h
  * @brief   Timing utility macros for measuring function execution time
  ******************************************************************************
  * Usage examples:
  *
  *   // Measure a void function or statement
  *   uint32_t elapsed_ms;
  *   TIME_IT(elapsed_ms, my_void_function(arg1, arg2));
  *   LOG_INFO_TAG("PERF", "Took %lu ms", (unsigned long)elapsed_ms);
  *
  *   // Measure a function and capture its return value
  *   uint32_t elapsed_ms;
  *   int result;
  *   TIME_IT_RET(elapsed_ms, result, my_function(arg1, arg2));
  *
  *   // Measure and log in one step (uses LOG_INFO_TAG)
  *   TIME_IT_LOG("MyFunc", my_function(arg1, arg2));
  *
  ******************************************************************************
  */
#ifndef TIME_IT_H
#define TIME_IT_H

#include "stm32h5xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Measure execution time of a function/statement.
  * @param  elapsed_ms  Variable to store elapsed time in milliseconds (uint32_t)
  * @param  func_call   The function call or statement to measure
  * @note   Uses HAL_GetTick() which has 1ms resolution
  *
  * Example:
  *   uint32_t ms;
  *   TIME_IT(ms, process_data(buffer, len));
  */
#define TIME_IT(elapsed_ms, func_call) \
    do { \
        uint32_t _time_it_start = HAL_GetTick(); \
        func_call; \
        uint32_t _time_it_end = HAL_GetTick(); \
        (elapsed_ms) = (_time_it_end - _time_it_start); \
    } while(0)

/**
  * @brief  Measure execution time and capture return value.
  * @param  elapsed_ms  Variable to store elapsed time in milliseconds (uint32_t)
  * @param  ret_val     Variable to store the function's return value
  * @param  func_call   The function call to measure
  *
  * Example:
  *   uint32_t ms;
  *   int status;
  *   TIME_IT_RET(ms, status, initialize_hardware());
  */
#define TIME_IT_RET(elapsed_ms, ret_val, func_call) \
    do { \
        uint32_t _time_it_start = HAL_GetTick(); \
        (ret_val) = (func_call); \
        uint32_t _time_it_end = HAL_GetTick(); \
        (elapsed_ms) = (_time_it_end - _time_it_start); \
    } while(0)

/**
  * @brief  Measure execution time using DWT cycle counter for microsecond precision.
  * @param  elapsed_us  Variable to store elapsed time in microseconds (uint32_t)
  * @param  func_call   The function call or statement to measure
  * @note   Requires DWT to be enabled. More precise than TIME_IT for short operations.
  * @note   Maximum measurable time depends on CPU frequency (~17 seconds at 250MHz)
  *
  * Example:
  *   uint32_t us;
  *   TIME_IT_US(us, fast_operation());
  */
#define TIME_IT_US(elapsed_us, func_call) \
    do { \
        /* Enable DWT if not already enabled */ \
        if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) { \
            CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; \
        } \
        if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) { \
            DWT->CYCCNT = 0U; \
            DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; \
        } \
        uint32_t _time_it_start = DWT->CYCCNT; \
        func_call; \
        uint32_t _time_it_end = DWT->CYCCNT; \
        uint32_t _time_it_cycles = _time_it_end - _time_it_start; \
        uint32_t _time_it_freq = HAL_RCC_GetHCLKFreq(); \
        (elapsed_us) = (_time_it_cycles / (_time_it_freq / 1000000U)); \
    } while(0)

/**
  * @brief  Measure execution time with microsecond precision and capture return value.
  * @param  elapsed_us  Variable to store elapsed time in microseconds (uint32_t)
  * @param  ret_val     Variable to store the function's return value
  * @param  func_call   The function call to measure
  */
#define TIME_IT_US_RET(elapsed_us, ret_val, func_call) \
    do { \
        if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) { \
            CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; \
        } \
        if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U) { \
            DWT->CYCCNT = 0U; \
            DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; \
        } \
        uint32_t _time_it_start = DWT->CYCCNT; \
        (ret_val) = (func_call); \
        uint32_t _time_it_end = DWT->CYCCNT; \
        uint32_t _time_it_cycles = _time_it_end - _time_it_start; \
        uint32_t _time_it_freq = HAL_RCC_GetHCLKFreq(); \
        (elapsed_us) = (_time_it_cycles / (_time_it_freq / 1000000U)); \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* TIME_IT_H */
