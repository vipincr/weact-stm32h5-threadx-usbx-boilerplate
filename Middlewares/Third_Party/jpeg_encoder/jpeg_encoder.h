#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Demosaic Constants matched to calibrated values
#define JPEG_DEMOSAIC_RED_GAIN 1.375000f
#define JPEG_DEMOSAIC_GREEN_GAIN 0.970000f
#define JPEG_DEMOSAIC_BLUE_GAIN 1.200000f
#define JPEG_DEMOSAIC_RED_GAIN_Q8  ((int)(JPEG_DEMOSAIC_RED_GAIN * 256.0f + 0.5f))
#define JPEG_DEMOSAIC_GREEN_GAIN_Q8  ((int)(JPEG_DEMOSAIC_GREEN_GAIN * 256.0f + 0.5f))
#define JPEG_DEMOSAIC_BLUE_GAIN_Q8 ((int)(JPEG_DEMOSAIC_BLUE_GAIN * 256.0f + 0.5f))

// Memory Safety Limit (Default: 64KB)
#ifndef JPEG_ENCODER_MAX_MEMORY_USAGE
#define JPEG_ENCODER_MAX_MEMORY_USAGE (128 * 1024)
#endif

/**
 * @brief Bayer patterns for raw image data.
 */
typedef enum {
    JPEG_BAYER_PATTERN_RGGB = 0,
    JPEG_BAYER_PATTERN_BGGR,
    JPEG_BAYER_PATTERN_GRBG,
    JPEG_BAYER_PATTERN_GBRG
} jpeg_bayer_pattern_t;

/**
 * @brief Pixel formats.
 */
typedef enum {
    JPEG_PIXEL_FORMAT_UNKNOWN = 0,
    JPEG_PIXEL_FORMAT_BAYER12_GRGB, // 16-bit unpacked, MSB aligned usually
    JPEG_PIXEL_FORMAT_PACKED10,
    JPEG_PIXEL_FORMAT_UNPACKED10,
    JPEG_PIXEL_FORMAT_PACKED12,
    JPEG_PIXEL_FORMAT_UNPACKED12,
    JPEG_PIXEL_FORMAT_UNPACKED16,
    JPEG_PIXEL_FORMAT_UNPACKED8
} jpeg_pixel_format_t;

/**
 * @brief Chroma subsampling modes.
 */
typedef enum {
    JPEG_SUBSAMPLE_444 = 0,
    JPEG_SUBSAMPLE_420,
    JPEG_SUBSAMPLE_422
} jpeg_subsample_t;

/**
 * @brief Stream interface for reading/writing data.
 */
typedef struct {
    size_t (*read)(void* ctx, void* buf, size_t size);
    void* read_ctx;
    size_t (*write)(void* ctx, const void* buf, size_t size);
    void* write_ctx;
} jpeg_stream_t;

/**
 * @brief Detailed error codes for JPEG encoder.
 *        Each failure path returns a unique negative code.
 */
typedef enum {
    JPEG_ENCODER_ERR_OK = 0,
    JPEG_ENCODER_ERR_INVALID_ARGUMENT = 1,
    JPEG_ENCODER_ERR_INVALID_DIMENSIONS = 2,
    JPEG_ENCODER_ERR_INVALID_STRIDE = 3,
    JPEG_ENCODER_ERR_MEMORY_LIMIT_EXCEEDED = 4,
    JPEG_ENCODER_ERR_OFFSET_EOF = 5,
    JPEG_ENCODER_ERR_JPEG_INIT_FAILED = 6,
    JPEG_ENCODER_ERR_ALLOC_RAW_BUFFER = 7,
    JPEG_ENCODER_ERR_ALLOC_UNPACK_BUFFER = 8,
    JPEG_ENCODER_ERR_ALLOC_RGB_BUFFER = 9,
    JPEG_ENCODER_ERR_ALLOC_CARRY_BUFFER = 10,
    JPEG_ENCODER_ERR_ALLOC_LOOKAHEAD_BUFFER = 11,
    JPEG_ENCODER_ERR_WRITE_OVERFLOW = 12,
    JPEG_ENCODER_ERR_NULL_OUT_SIZE = 13,
    JPEG_ENCODER_ERR_NULL_IN_BUFFER = 14,
    JPEG_ENCODER_ERR_NULL_OUT_BUFFER = 15,
    JPEG_ENCODER_ERR_ZERO_OUT_CAPACITY = 16
} jpeg_encoder_error_code_t;

/**
 * @brief Error detail structure.
 */
typedef struct {
    jpeg_encoder_error_code_t code;
    const char* message;
    const char* function;
    int line;
} jpeg_encoder_error_t;

/**
 * @brief Configuration for the JPEG encoder.
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    jpeg_pixel_format_t pixel_format;
    
    // Raw Processing Params
    jpeg_bayer_pattern_t bayer_pattern;
    bool subtract_ob;
    uint16_t ob_value;
    bool apply_awb;
    float awb_r_gain; // optional override when apply_awb is true
    float awb_g_gain; // optional override when apply_awb is true
    float awb_b_gain; // optional override when apply_awb is true
    
    // JPEG Specific
    int quality; // 0-100

    // Stream Processing
    int start_offset_lines; // Skip these many lines from start of stream
    
    // Optimizations
    bool enable_fast_mode; // Enable SIMD/Fixed-Point Paths if available

    // Subsampling
    jpeg_subsample_t subsample; // 4:4:4 (default), 4:2:0, or 4:2:2
    
} jpeg_encoder_config_t;


/**
 * @brief Compress a stream of raw data to JPEG.
 * 
 * @param stream  Input/Output stream interface
 * @param config  Compression configuration
 * @return 0 on success, negative on error (unique per failure path).
 */
int jpeg_encode_stream(jpeg_stream_t* stream, const jpeg_encoder_config_t* config);

/**
 * @brief Compress a raw memory buffer to JPEG in another memory buffer.
 * 
 * @param in_buf Source buffer containing raw data
 * @param in_size Size of source buffer
 * @param out_buf Destination buffer for JPEG data
 * @param out_capacity Size of destination buffer
 * @param out_size [Out] Actual size of generated JPEG
 * @param config Compression configuration
 * @return 0 on success, negative on error (unique per failure path).
 */
int jpeg_encode_buffer(const uint8_t* in_buf, size_t in_size, uint8_t* out_buf, size_t out_capacity, size_t* out_size, const jpeg_encoder_config_t* config);

/**
 * @brief Compress a stream of raw data using delta + Golomb/Rice.
 * 
 * @param stream  Input/Output stream interface
 * @param config  Raw input configuration (width/height/pixel_format)
 * @param rice_cfg Rice encoder configuration
 * @param out_size [Out] Number of bytes written (optional; can be NULL)
 * @return 0 on success, negative on error.
 */

/**
 * @brief Calculate the memory required by the encoder for a given configuration.
 * 
 * @param config Configuration to check
 * @return Size in bytes
 */
size_t jpeg_encoder_estimate_memory_requirement(const jpeg_encoder_config_t* config);

/**
 * @brief Retrieve the last error that occurred in the encoder.
 * 
 * @param out_error Output structure for error details
 * @return 0 on success, negative on invalid arguments.
 */
int jpeg_encoder_get_last_error(jpeg_encoder_error_t* out_error);

#ifdef __cplusplus
}
#endif
