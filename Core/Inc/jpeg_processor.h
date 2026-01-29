/**
  ******************************************************************************
  * @file    jpeg_processor.h
  * @brief   JPEG Processor - Converts Bayer RAW .bin files to JPEG
  ******************************************************************************
  * This module integrates with the filesystem change notifier to automatically
  * encode .bin files (Bayer RAW) to JPEG format when they are created or modified.
  ******************************************************************************
  */
#ifndef JPEG_PROCESSOR_H
#define JPEG_PROCESSOR_H

#include "tx_api.h"
#include "fs_reader.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration ------------------------------------------------------------ */

/* Default image dimensions for Bayer RAW files */
#ifndef JPEG_PROCESSOR_DEFAULT_WIDTH
#define JPEG_PROCESSOR_DEFAULT_WIDTH    640
#endif

#ifndef JPEG_PROCESSOR_DEFAULT_HEIGHT
#define JPEG_PROCESSOR_DEFAULT_HEIGHT   400
#endif

/* JPEG quality (0-100) */
#ifndef JPEG_PROCESSOR_QUALITY
#define JPEG_PROCESSOR_QUALITY          90
#endif

/* Maximum file size to process (to avoid memory issues) */
#ifndef JPEG_PROCESSOR_MAX_FILE_SIZE
#define JPEG_PROCESSOR_MAX_FILE_SIZE    (2 * 1024 * 1024)  /* 2 MB */
#endif

/* Public types ------------------------------------------------------------- */

/**
  * @brief  JPEG processor status codes.
  */
typedef enum {
    JPEG_PROC_OK = 0,
    JPEG_PROC_ERR_NOT_INITIALIZED,
    JPEG_PROC_ERR_FILE_TOO_LARGE,
    JPEG_PROC_ERR_OPEN_INPUT,
    JPEG_PROC_ERR_READ_INPUT,
    JPEG_PROC_ERR_ALLOC,
    JPEG_PROC_ERR_ENCODE,
    JPEG_PROC_ERR_CREATE_OUTPUT,
    JPEG_PROC_ERR_WRITE_OUTPUT,
    JPEG_PROC_ERR_FS_NOT_MOUNTED
} JPEG_Processor_Status_t;

/**
  * @brief  Configuration for the JPEG processor.
  */
typedef struct {
    uint16_t width;              /**< Image width in pixels */
    uint16_t height;             /**< Image height in pixels */
    int      quality;            /**< JPEG quality (0-100) */
    int      start_offset_lines; /**< Lines to skip at start of RAW data */
    int      enable_fast_mode;   /**< Use optimized fixed-point encoding */
} JPEG_Processor_Config_t;

/* Public functions --------------------------------------------------------- */

/**
  * @brief  Initialize the JPEG processor.
  *         This sets up the filesystem change callback.
  * @retval JPEG_PROC_OK on success.
  */
JPEG_Processor_Status_t JPEG_Processor_Init(void);

/**
  * @brief  Process a single .bin file and convert to JPEG.
  * @param  bin_path  Full path to the .bin file (e.g., "/folder/image.bin")
  * @param  config    Optional configuration (NULL for defaults)
  * @retval JPEG_PROC_OK on success, error code otherwise.
  */
JPEG_Processor_Status_t JPEG_Processor_ConvertFile(const char *bin_path, 
                                                    const JPEG_Processor_Config_t *config);

/**
  * @brief  Get the last encoding time in milliseconds.
  * @retval Time in milliseconds for the last successful encoding.
  */
uint32_t JPEG_Processor_GetLastEncodingTime(void);

/**
  * @brief  Get the last output file size in bytes.
  * @retval Size in bytes of the last successfully created JPEG.
  */
size_t JPEG_Processor_GetLastOutputSize(void);

/**
  * @brief  Check if a file path has a .bin extension.
  * @param  path  File path to check
  * @retval 1 if .bin file, 0 otherwise.
  */
int JPEG_Processor_IsBinFile(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_PROCESSOR_H */
