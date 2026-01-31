/**
  ******************************************************************************
  * @file    jpeg_encoder_timing.h
  * @brief   JPEG Encoder Timing Instrumentation
  ******************************************************************************
  * Provides macros and accumulators to measure time spent in each encoding stage.
  * Uses DWT cycle counter for microsecond precision.
  * 
  * Usage:
  *   1. Call JPEG_TIMING_INIT() once at start
  *   2. Use JPEG_TIMING_START(stage) and JPEG_TIMING_END(stage) around code
  *   3. Call JPEG_TIMING_REPORT() to print results
  ******************************************************************************
  */
#ifndef JPEG_ENCODER_TIMING_H
#define JPEG_ENCODER_TIMING_H

#include <stdint.h>

/* Enable/disable timing (set to 0 for production builds) */
#ifndef JPEG_TIMING_ENABLED
#define JPEG_TIMING_ENABLED 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Timing stages - each major step in the encoding pipeline */
typedef enum {
    JPEG_TIMING_RAW_READ = 0,       /* Reading raw data from stream */
    JPEG_TIMING_UNPACK,             /* Unpacking packed Bayer data */
    JPEG_TIMING_DEMOSAIC,           /* Bayer demosaic to RGB/YUV */
    JPEG_TIMING_COLOR_CONVERT,      /* RGB to YCbCr conversion (if separate) */
    JPEG_TIMING_MCU_PREPARE,        /* Preparing MCU data */
    JPEG_TIMING_DCT,                /* Forward DCT transform */
    JPEG_TIMING_QUANTIZE,           /* Quantization */
    JPEG_TIMING_HUFFMAN,            /* Huffman encoding */
    JPEG_TIMING_STREAM_WRITE,       /* Writing to output stream */
    JPEG_TIMING_OVERHEAD,           /* Loop/control overhead */
    JPEG_TIMING_COUNT               /* Number of stages */
} jpeg_timing_stage_t;

/* Timing data structure */
typedef struct {
    uint32_t cycles[JPEG_TIMING_COUNT];     /* Accumulated cycles per stage */
    uint32_t calls[JPEG_TIMING_COUNT];      /* Number of times each stage was measured */
    uint32_t temp_start;                     /* Temporary start cycle for current measurement */
    uint32_t total_start;                    /* Total frame start time */
    uint32_t total_cycles;                   /* Total frame cycles */
    uint32_t cpu_freq_mhz;                   /* CPU frequency for cycle->us conversion */
} jpeg_timing_t;

#if JPEG_TIMING_ENABLED

/* Global timing data - defined in jpeg_encoder.c */
extern jpeg_timing_t g_jpeg_timing;

/**
 * @brief  Initialize the timing infrastructure.
 *         Enables DWT cycle counter if not already enabled.
 */
static inline void jpeg_timing_init(void)
{
    /* Enable DWT CYCCNT if not already enabled */
    volatile uint32_t *demcr = (volatile uint32_t *)0xE000EDFC;
    volatile uint32_t *dwt_ctrl = (volatile uint32_t *)0xE0001000;
    volatile uint32_t *dwt_cyccnt = (volatile uint32_t *)0xE0001004;
    
    if ((*demcr & 0x01000000) == 0) {
        *demcr |= 0x01000000;  /* Enable trace */
    }
    if ((*dwt_ctrl & 1) == 0) {
        *dwt_cyccnt = 0;
        *dwt_ctrl |= 1;  /* Enable CYCCNT */
    }
    
    /* Reset all counters */
    for (int i = 0; i < JPEG_TIMING_COUNT; i++) {
        g_jpeg_timing.cycles[i] = 0;
        g_jpeg_timing.calls[i] = 0;
    }
    g_jpeg_timing.total_cycles = 0;
    
    /* Estimate CPU frequency (assume 250MHz for STM32H5) */
    g_jpeg_timing.cpu_freq_mhz = 250;
}

/**
 * @brief  Get current DWT cycle count.
 */
static inline uint32_t jpeg_timing_get_cycles(void)
{
    volatile uint32_t *dwt_cyccnt = (volatile uint32_t *)0xE0001004;
    return *dwt_cyccnt;
}

/**
 * @brief  Mark the start of total frame timing.
 */
static inline void jpeg_timing_frame_start(void)
{
    g_jpeg_timing.total_start = jpeg_timing_get_cycles();
}

/**
 * @brief  Mark the end of total frame timing.
 */
static inline void jpeg_timing_frame_end(void)
{
    g_jpeg_timing.total_cycles = jpeg_timing_get_cycles() - g_jpeg_timing.total_start;
}

/**
 * @brief  Start timing a specific stage.
 */
static inline void jpeg_timing_start(jpeg_timing_stage_t stage)
{
    (void)stage;  /* Stage stored implicitly by caller */
    g_jpeg_timing.temp_start = jpeg_timing_get_cycles();
}

/**
 * @brief  End timing a specific stage and accumulate.
 */
static inline void jpeg_timing_end(jpeg_timing_stage_t stage)
{
    uint32_t elapsed = jpeg_timing_get_cycles() - g_jpeg_timing.temp_start;
    g_jpeg_timing.cycles[stage] += elapsed;
    g_jpeg_timing.calls[stage]++;
}

/**
 * @brief  Convert cycles to microseconds.
 */
static inline uint32_t jpeg_timing_cycles_to_us(uint32_t cycles)
{
    return cycles / g_jpeg_timing.cpu_freq_mhz;
}

/**
 * @brief  Convert cycles to milliseconds.
 */
static inline uint32_t jpeg_timing_cycles_to_ms(uint32_t cycles)
{
    return cycles / (g_jpeg_timing.cpu_freq_mhz * 1000);
}

/**
 * @brief  Get the name of a timing stage.
 */
static inline const char* jpeg_timing_stage_name(jpeg_timing_stage_t stage)
{
    static const char* names[] = {
        "RAW_READ",
        "UNPACK",
        "DEMOSAIC",
        "COLOR_CVT",
        "MCU_PREP",
        "DCT",
        "QUANTIZE",
        "HUFFMAN",
        "STREAM_WR",
        "OVERHEAD"
    };
    if (stage < JPEG_TIMING_COUNT) {
        return names[stage];
    }
    return "UNKNOWN";
}

/* Convenience macros */
#define JPEG_TIMING_INIT()              jpeg_timing_init()
#define JPEG_TIMING_RESET()             jpeg_timing_init()
#define JPEG_TIMING_FRAME_START()       jpeg_timing_frame_start()
#define JPEG_TIMING_FRAME_END()         jpeg_timing_frame_end()
#define JPEG_TIMING_START(stage)        jpeg_timing_start(stage)
#define JPEG_TIMING_END(stage)          jpeg_timing_end(stage)
#define JPEG_TIMING_CYCLES(stage)       (g_jpeg_timing.cycles[stage])
#define JPEG_TIMING_CALLS(stage)        (g_jpeg_timing.calls[stage])
#define JPEG_TIMING_TOTAL_CYCLES()      (g_jpeg_timing.total_cycles)
#define JPEG_TIMING_TO_US(cycles)       jpeg_timing_cycles_to_us(cycles)
#define JPEG_TIMING_TO_MS(cycles)       jpeg_timing_cycles_to_ms(cycles)
#define JPEG_TIMING_STAGE_NAME(stage)   jpeg_timing_stage_name(stage)

#else /* JPEG_TIMING_ENABLED == 0 */

/* Stubs when timing is disabled */
#define JPEG_TIMING_INIT()              do {} while(0)
#define JPEG_TIMING_RESET()             do {} while(0)
#define JPEG_TIMING_FRAME_START()       do {} while(0)
#define JPEG_TIMING_FRAME_END()         do {} while(0)
#define JPEG_TIMING_START(stage)        do {} while(0)
#define JPEG_TIMING_END(stage)          do {} while(0)
#define JPEG_TIMING_CYCLES(stage)       (0)
#define JPEG_TIMING_CALLS(stage)        (0)
#define JPEG_TIMING_TOTAL_CYCLES()      (0)
#define JPEG_TIMING_TO_US(cycles)       (0)
#define JPEG_TIMING_TO_MS(cycles)       (0)
#define JPEG_TIMING_STAGE_NAME(stage)   ""

#endif /* JPEG_TIMING_ENABLED */

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ENCODER_TIMING_H */
