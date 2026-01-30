/**
  ******************************************************************************
  * @file    jpeg_processor.c
  * @brief   JPEG Processor - Converts Bayer RAW .bin files to JPEG
  ******************************************************************************
  * This module uses streaming encoding to minimize RAM usage.
  * Files are read and written directly via FatFS without large buffers.
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

/* Private defines -----------------------------------------------------------*/
#define JPEG_PROC_TAG  "JPEG"

/* Stream context for FatFS file I/O */
typedef struct {
    FIL *fin;              /* Input file handle */
    FIL *fout;             /* Output file handle */
    size_t bytes_written;  /* Track output size */
} jpeg_stream_ctx_t;

/* Private variables ---------------------------------------------------------*/
static int jpeg_proc_initialized = 0;
static uint32_t last_encoding_time_ms = 0;
static size_t last_output_size = 0;

/* Debug counters for stream callbacks */
static volatile uint32_t read_call_count = 0;
static volatile uint32_t read_total_bytes = 0;

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
static size_t jpeg_stream_read(void *ctx, void *buf, size_t size);
static size_t jpeg_stream_write(void *ctx, const void *buf, size_t size);

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

int JPEG_Processor_IsInitialized(void)
{
    return jpeg_proc_initialized ? 1 : 0;
}

JPEG_Processor_Status_t JPEG_Processor_ConvertFile(const char *bin_path,
                                                    const JPEG_Processor_Config_t *config)
{
    FRESULT fres;
    FIL fin, fout;
    FSIZE_t file_size;
    char jpg_path[128];
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
    LOG_DEBUG_TAG(JPEG_PROC_TAG, "Opening input file...");
    fres = f_open(&fin, bin_path, FA_READ);
    if (fres != FR_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Open input failed: %s (err=%d)", bin_path, fres);
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
    
    /* Open output file */
    fres = f_open(&fout, jpg_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fres != FR_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Create output failed: %s (err=%d)", jpg_path, fres);
        f_close(&fin);
        return JPEG_PROC_ERR_CREATE_OUTPUT;
    }
    
    /* Set up stream context */
    jpeg_stream_ctx_t stream_ctx = {
        .fin = &fin,
        .fout = &fout,
        .bytes_written = 0
    };
    
    /* Set up stream interface */
    jpeg_stream_t stream = {
        .read = jpeg_stream_read,
        .read_ctx = &stream_ctx,
        .write = jpeg_stream_write,
        .write_ctx = &stream_ctx
    };
    
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
    
    /* Check memory requirements before encoding */
    size_t mem_req = jpeg_encoder_estimate_memory_requirement(&enc_config);
    LOG_DEBUG_TAG(JPEG_PROC_TAG, "Memory required: %lu bytes", (unsigned long)mem_req);
    
    /* Reset read counters */
    read_call_count = 0;
    read_total_bytes = 0;
    
    /* Encode using streaming (low memory usage) */
    LOG_DEBUG_TAG(JPEG_PROC_TAG, "Starting encode...");
    TIME_IT(elapsed_ms, encode_result = jpeg_encode_stream(&stream, &enc_config));
    LOG_DEBUG_TAG(JPEG_PROC_TAG, "Encode returned: %d", encode_result);
    
    /* Close files */
    f_close(&fin);
    f_close(&fout);
    
    if (encode_result != 0)
    {
        jpeg_encoder_error_t err;
        jpeg_encoder_get_last_error(&err);
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Encode failed: %d (%s)", 
                      encode_result, err.message ? err.message : "unknown");
        /* Delete partial output file */
        f_unlink(jpg_path);
        return JPEG_PROC_ERR_ENCODE;
    }
    
    /* Update stats */
    last_encoding_time_ms = elapsed_ms;
    last_output_size = stream_ctx.bytes_written;
    
    /* Calculate compression ratio (integer math since nano libc doesn't support %f) */
    unsigned long ratio_x10 = (stream_ctx.bytes_written > 0) ? 
                  ((unsigned long)file_size * 10UL) / (unsigned long)stream_ctx.bytes_written : 0UL;
    
    LOG_INFO_TAG(JPEG_PROC_TAG, "Encoded: %s (%lu bytes, %lu.%lux, %lu ms)",
                 jpg_path, (unsigned long)stream_ctx.bytes_written, 
                 ratio_x10 / 10UL, ratio_x10 % 10UL, (unsigned long)elapsed_ms);
    
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
  * @brief  Stream read callback for FatFS.
  */
static size_t jpeg_stream_read(void *ctx, void *buf, size_t size)
{
    jpeg_stream_ctx_t *stream_ctx = (jpeg_stream_ctx_t *)ctx;
    UINT bytes_read = 0;
    
    if (stream_ctx == NULL || stream_ctx->fin == NULL || buf == NULL || size == 0)
    {
        return 0;
    }
    
    FRESULT res = f_read(stream_ctx->fin, buf, (UINT)size, &bytes_read);
    if (res != FR_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Stream read error: %d (call #%lu)", 
                      (int)res, (unsigned long)read_call_count);
        return 0;
    }
    
    read_call_count++;
    read_total_bytes += bytes_read;
    
    /* Log progress every 100 calls */
    if ((read_call_count % 100) == 0)
    {
        LOG_DEBUG_TAG(JPEG_PROC_TAG, "Read progress: %lu calls, %lu KB", 
                      (unsigned long)read_call_count, 
                      (unsigned long)(read_total_bytes / 1024));
    }
    
    return (size_t)bytes_read;
}

/**
  * @brief  Stream write callback for FatFS.
  */
static size_t jpeg_stream_write(void *ctx, const void *buf, size_t size)
{
    jpeg_stream_ctx_t *stream_ctx = (jpeg_stream_ctx_t *)ctx;
    UINT bytes_written = 0;
    
    if (stream_ctx == NULL || stream_ctx->fout == NULL || buf == NULL || size == 0)
    {
        return 0;
    }
    
    FRESULT res = f_write(stream_ctx->fout, buf, (UINT)size, &bytes_written);
    if (res != FR_OK)
    {
        LOG_ERROR_TAG(JPEG_PROC_TAG, "Stream write error: %d", (int)res);
        return 0;
    }
    
    stream_ctx->bytes_written += bytes_written;
    return (size_t)bytes_written;
}
