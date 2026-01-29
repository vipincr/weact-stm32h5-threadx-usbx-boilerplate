/**
  ******************************************************************************
  * @file    jpeg_processor.c
  * @brief   JPEG Processor - Converts Bayer RAW .bin files to JPEG
  ******************************************************************************
  * This module integrates with the filesystem change notifier to automatically
  * encode .bin files (Bayer RAW) to JPEG format when they are created or modified.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "jpeg_processor.h"
#include "jpeg_encoder.h"
#include "ff.h"
#include "fs_reader.h"
#include "logger.h"
#include "time_it.h"
#include <string.h>
#include <stdlib.h>

/* Private defines -----------------------------------------------------------*/
#define JPEG_PROC_TAG  "JPEG"

/* Private variables ---------------------------------------------------------*/
static int jpeg_proc_initialized = 0;
static uint32_t last_encoding_time_ms = 0;
static size_t last_output_size = 0;

/* Static buffers for encoding - large so must be static, not stack */
static uint8_t *jpeg_input_buffer = NULL;
static uint8_t *jpeg_output_buffer = NULL;
static size_t jpeg_input_buffer_size = 0;
static size_t jpeg_output_buffer_size = 0;

/* Default configuration */
static const JPEG_Processor_Config_t default_config = {
    .width = JPEG_PROCESSOR_DEFAULT_WIDTH,
    .height = JPEG_PROCESSOR_DEFAULT_HEIGHT,
    .quality = JPEG_PROCESSOR_QUALITY,
    .start_offset_lines = 2,
    .enable_fast_mode = 1
};

/* Private function prototypes -----------------------------------------------*/
static void jpeg_fs_change_handler(FS_EventType_t event_type, const char *path);
static int jpeg_build_output_path(char *out_path, size_t out_len, const char *bin_path);
static int jpeg_ensure_buffers(size_t input_size, size_t output_capacity);

/* Public functions ----------------------------------------------------------*/

int JPEG_Processor_IsBinFile(const char *path)
{
    if (path == NULL)
    {
        return 0;
    }
    
    size_t len = strlen(path);
    if (len < 5)  /* Minimum: "a.bin" */
    {
        return 0;
    }
    
    /* Check for .bin extension (case insensitive) */
    const char *ext = path + len - 4;
    if ((ext[0] == '.') &&
        (ext[1] == 'b' || ext[1] == 'B') &&
        (ext[2] == 'i' || ext[2] == 'I') &&
        (ext[3] == 'n' || ext[3] == 'N'))
    {
        return 1;
    }
    
    return 0;
}

JPEG_Processor_Status_t JPEG_Processor_Init(void)
{
    if (jpeg_proc_initialized)
    {
        return JPEG_PROC_OK;
    }
    
    /* Register our handler with the filesystem monitor */
    FS_Reader_SetChangeCallback(jpeg_fs_change_handler);
    
    jpeg_proc_initialized = 1;
    LOG_INFO_TAG(JPEG_PROC_TAG, "JPEG processor initialized");
    
    return JPEG_PROC_OK;
}

JPEG_Processor_Status_t JPEG_Processor_ConvertFile(const char *bin_path,
                                                    const JPEG_Processor_Config_t *config)
{
    FRESULT fres;
    FIL fin, fout;
    UINT bytes_read, bytes_written;
    FSIZE_t file_size;
    char jpg_path[128];
    size_t jpg_size = 0;
    int encode_result;
    uint32_t elapsed_ms = 0;
    
    if (!jpeg_proc_initialized)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Not initialized");
        return JPEG_PROC_ERR_NOT_INITIALIZED;
    }
    
    if (!FS_Reader_IsMounted())
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Filesystem not mounted");
        return JPEG_PROC_ERR_FS_NOT_MOUNTED;
    }
    
    /* Use default config if none provided */
    if (config == NULL)
    {
        config = &default_config;
    }
    
    /* Build output path (replace .bin with .jpg) */
    if (jpeg_build_output_path(jpg_path, sizeof(jpg_path), bin_path) != 0)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Path too long: %s", bin_path);
        return JPEG_PROC_ERR_OPEN_INPUT;
    }
    
    LOG_INFO_TAG(JPEG_PROC_TAG, "Processing: %s", bin_path);
    
    /* Open input file */
    fres = f_open(&fin, bin_path, FA_READ);
    if (fres != FR_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Open failed: %s (err=%d)", bin_path, fres);
        return JPEG_PROC_ERR_OPEN_INPUT;
    }
    
    file_size = f_size(&fin);
    
    /* Check file size limit */
    if (file_size > JPEG_PROCESSOR_MAX_FILE_SIZE)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "File too large: %lu bytes", (unsigned long)file_size);
        f_close(&fin);
        return JPEG_PROC_ERR_FILE_TOO_LARGE;
    }
    
    /* Estimate output size (worst case: uncompressed RGB) */
    size_t output_capacity = (size_t)(config->width * config->height * 3);
    
    /* Ensure we have buffers */
    if (jpeg_ensure_buffers((size_t)file_size, output_capacity) != 0)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Buffer allocation failed");
        f_close(&fin);
        return JPEG_PROC_ERR_ALLOC;
    }
    
    /* Read entire input file */
    fres = f_read(&fin, jpeg_input_buffer, (UINT)file_size, &bytes_read);
    f_close(&fin);
    
    if (fres != FR_OK || bytes_read != (UINT)file_size)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Read failed: %d bytes (expected %lu)", 
                      bytes_read, (unsigned long)file_size);
        return JPEG_PROC_ERR_READ_INPUT;
    }
    
    /* Configure encoder */
    jpeg_encoder_config_t enc_config;
    memset(&enc_config, 0, sizeof(enc_config));
    enc_config.width = config->width;
    enc_config.height = config->height;
    enc_config.pixel_format = JPEG_PIXEL_FORMAT_BAYER12_GRGB;
    enc_config.bayer_pattern = JPEG_BAYER_PATTERN_GBRG;
    enc_config.quality = config->quality;
    enc_config.start_offset_lines = config->start_offset_lines;
    enc_config.apply_awb = true;
    enc_config.awb_r_gain = JPEG_DEMOSAIC_RED_GAIN;
    enc_config.awb_g_gain = JPEG_DEMOSAIC_GREEN_GAIN;
    enc_config.awb_b_gain = JPEG_DEMOSAIC_BLUE_GAIN;
    enc_config.enable_fast_mode = config->enable_fast_mode ? true : false;
    enc_config.subsample = JPEG_SUBSAMPLE_420;  /* Good compression/quality balance */
    
    /* Encode with timing */
    TIME_IT(elapsed_ms, 
            encode_result = jpeg_encode_buffer(jpeg_input_buffer, (size_t)file_size,
                                               jpeg_output_buffer, output_capacity,
                                               &jpg_size, &enc_config));
    
    if (encode_result != 0)
    {
        jpeg_encoder_error_t err;
        jpeg_encoder_get_last_error(&err);
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Encode failed: %d (%s)", 
                      encode_result, err.message ? err.message : "unknown");
        return JPEG_PROC_ERR_ENCODE;
    }
    
    /* Write output file */
    fres = f_open(&fout, jpg_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fres != FR_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Create output failed: %s (err=%d)", jpg_path, fres);
        return JPEG_PROC_ERR_CREATE_OUTPUT;
    }
    
    fres = f_write(&fout, jpeg_output_buffer, (UINT)jpg_size, &bytes_written);
    f_close(&fout);
    
    if (fres != FR_OK || bytes_written != (UINT)jpg_size)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Write failed: %d bytes (expected %zu)", 
                      bytes_written, jpg_size);
        return JPEG_PROC_ERR_WRITE_OUTPUT;
    }
    
    /* Update stats */
    last_encoding_time_ms = elapsed_ms;
    last_output_size = jpg_size;
    
    /* Calculate compression ratio */
    float ratio = (jpg_size > 0) ? (float)file_size / (float)jpg_size : 0.0f;
    
    LOG_INFO_TAG(JPEG_PROC_TAG, "Encoded: %s (%zu bytes, %.1fx, %lu ms)",
                 jpg_path, jpg_size, ratio, (unsigned long)elapsed_ms);
    
    return JPEG_PROC_OK;
}

uint32_t JPEG_Processor_GetLastEncodingTime(void)
{
    return last_encoding_time_ms;
}

size_t JPEG_Processor_GetLastOutputSize(void)
{
    return last_output_size;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Filesystem change handler - processes .bin files.
  */
static void jpeg_fs_change_handler(FS_EventType_t event_type, const char *path)
{
    /* First, log the change using the default handler behavior */
    const char *event_str = FS_Reader_EventTypeStr(event_type);
    LOG_INFO_TAG("FS", "[%s] %s", event_str, path);
    
    /* Only process .bin files for creation/modification events */
    if (event_type != FS_EVENT_FILE_CREATED && event_type != FS_EVENT_FILE_MODIFIED)
    {
        return;
    }
    
    if (!JPEG_Processor_IsBinFile(path))
    {
        return;
    }
    
    /* Process the .bin file */
    LOG_INFO_TAG(JPEG_PROC_TAG, "Detected RAW file: %s", path);
    
    JPEG_Processor_Status_t status = JPEG_Processor_ConvertFile(path, NULL);
    if (status != JPEG_PROC_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Conversion failed: %d", (int)status);
    }
}

/**
  * @brief  Build output JPEG path from input .bin path.
  * @param  out_path  Buffer for output path
  * @param  out_len   Size of output buffer
  * @param  bin_path  Input .bin file path
  * @retval 0 on success, -1 on error
  */
static int jpeg_build_output_path(char *out_path, size_t out_len, const char *bin_path)
{
    size_t path_len = strlen(bin_path);
    
    if (path_len < 4 || path_len >= out_len)
    {
        return -1;
    }
    
    /* Copy path without the last 4 characters (.bin) */
    memcpy(out_path, bin_path, path_len - 4);
    
    /* Append .jpg */
    memcpy(out_path + path_len - 4, ".jpg", 5);  /* Including null terminator */
    
    return 0;
}

/**
  * @brief  Ensure input/output buffers are allocated and large enough.
  * @param  input_size      Required input buffer size
  * @param  output_capacity Required output buffer capacity
  * @retval 0 on success, -1 on allocation failure
  */
static int jpeg_ensure_buffers(size_t input_size, size_t output_capacity)
{
    /* Reallocate input buffer if needed */
    if (jpeg_input_buffer == NULL || jpeg_input_buffer_size < input_size)
    {
        uint8_t *new_buf = (uint8_t *)realloc(jpeg_input_buffer, input_size);
        if (new_buf == NULL)
        {
            return -1;
        }
        jpeg_input_buffer = new_buf;
        jpeg_input_buffer_size = input_size;
    }
    
    /* Reallocate output buffer if needed */
    if (jpeg_output_buffer == NULL || jpeg_output_buffer_size < output_capacity)
    {
        uint8_t *new_buf = (uint8_t *)realloc(jpeg_output_buffer, output_capacity);
        if (new_buf == NULL)
        {
            return -1;
        }
        jpeg_output_buffer = new_buf;
        jpeg_output_buffer_size = output_capacity;
    }
    
    return 0;
}
