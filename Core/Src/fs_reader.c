/**
  ******************************************************************************
  * @file    fs_reader.c
  * @brief   Filesystem reader - ThreadX wrapper for FatFs exFAT access
  ******************************************************************************
  * This module provides a ThreadX thread that mounts the SD card's exFAT
  * filesystem and lists the root directory contents via the CDC logger.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "fs_reader.h"
#include "ff.h"
#include "logger.h"
#include "sdmmc.h"
#include "tx_api.h"
#include <string.h>
#include <stdio.h>

/* Private defines -----------------------------------------------------------*/
#define FS_READER_THREAD_STACK_SIZE   4096U  /* Increased for FatFs + exFAT */
#define FS_READER_THREAD_PRIORITY     25U  /* Lower priority than USB (10) */

/* Private variables ---------------------------------------------------------*/
static TX_THREAD fs_reader_thread;
static UCHAR fs_reader_thread_stack[FS_READER_THREAD_STACK_SIZE];

static FATFS SDFatFs;       /* FatFs filesystem object */
static volatile int fs_mounted = 0;

/* Private function prototypes -----------------------------------------------*/
static VOID fs_reader_thread_entry(ULONG thread_input);
static void fs_list_directory(const char *path);
static const char* fs_result_str(FRESULT res);

/* Public functions ----------------------------------------------------------*/

/**
  * @brief  Initialize the filesystem reader thread.
  */
UINT FS_Reader_Init(TX_BYTE_POOL *byte_pool)
{
    UINT status;
    VOID *stack_ptr;

    (void)byte_pool;  /* Using static allocation */
    stack_ptr = fs_reader_thread_stack;

    status = tx_thread_create(&fs_reader_thread,
                              "FS Reader",
                              fs_reader_thread_entry,
                              0,
                              stack_ptr,
                              FS_READER_THREAD_STACK_SIZE,
                              FS_READER_THREAD_PRIORITY,
                              FS_READER_THREAD_PRIORITY,
                              TX_NO_TIME_SLICE,
                              TX_AUTO_START);

    if (status != TX_SUCCESS)
    {
        LOG_ERROR_TAG("FS", "Failed to create FS reader thread: %u", (unsigned)status);
    }

    return status;
}

/**
  * @brief  Check if filesystem is mounted.
  */
int FS_Reader_IsMounted(void)
{
    return fs_mounted;
}

/**
  * @brief  List contents of a directory.
  */
int FS_Reader_ListDir(const char *path)
{
    if (!fs_mounted)
    {
        LOG_ERROR_TAG("FS", "Filesystem not mounted");
        return -1;
    }

    fs_list_directory(path);
    return 0;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Convert FRESULT to string for logging.
  */
static const char* fs_result_str(FRESULT res)
{
    switch (res)
    {
        case FR_OK:                  return "OK";
        case FR_DISK_ERR:            return "DISK_ERR";
        case FR_INT_ERR:             return "INT_ERR";
        case FR_NOT_READY:           return "NOT_READY";
        case FR_NO_FILE:             return "NO_FILE";
        case FR_NO_PATH:             return "NO_PATH";
        case FR_INVALID_NAME:        return "INVALID_NAME";
        case FR_DENIED:              return "DENIED";
        case FR_EXIST:               return "EXIST";
        case FR_INVALID_OBJECT:      return "INVALID_OBJECT";
        case FR_WRITE_PROTECTED:     return "WRITE_PROTECTED";
        case FR_INVALID_DRIVE:       return "INVALID_DRIVE";
        case FR_NOT_ENABLED:         return "NOT_ENABLED";
        case FR_NO_FILESYSTEM:       return "NO_FILESYSTEM";
        case FR_MKFS_ABORTED:        return "MKFS_ABORTED";
        case FR_TIMEOUT:             return "TIMEOUT";
        case FR_LOCKED:              return "LOCKED";
        case FR_NOT_ENOUGH_CORE:     return "NOT_ENOUGH_CORE";
        case FR_TOO_MANY_OPEN_FILES: return "TOO_MANY_OPEN_FILES";
        case FR_INVALID_PARAMETER:   return "INVALID_PARAMETER";
        default:                     return "UNKNOWN";
    }
}

/**
  * @brief  Format file size as human-readable string.
  */
static void format_size(FSIZE_t size, char *buf, size_t buf_len)
{
    if (size >= (1024ULL * 1024ULL * 1024ULL))
    {
        snprintf(buf, buf_len, "%lu.%lu GB",
                 (unsigned long)(size / (1024ULL * 1024ULL * 1024ULL)),
                 (unsigned long)((size % (1024ULL * 1024ULL * 1024ULL)) / (100ULL * 1024ULL * 1024ULL)));
    }
    else if (size >= (1024ULL * 1024ULL))
    {
        snprintf(buf, buf_len, "%lu.%lu MB",
                 (unsigned long)(size / (1024ULL * 1024ULL)),
                 (unsigned long)((size % (1024ULL * 1024ULL)) / (100ULL * 1024ULL)));
    }
    else if (size >= 1024ULL)
    {
        snprintf(buf, buf_len, "%lu KB", (unsigned long)(size / 1024ULL));
    }
    else
    {
        snprintf(buf, buf_len, "%lu B", (unsigned long)size);
    }
}

/**
  * @brief  List directory contents to logger.
  */
static void fs_list_directory(const char *path)
{
    FRESULT res;
    DIR dir;
    FILINFO fno;
    int file_count = 0;
    int dir_count = 0;
    char size_str[32];

    res = f_opendir(&dir, path);
    if (res != FR_OK)
    {
        LOG_ERROR_TAG("FS", "opendir failed: %s", fs_result_str(res));
        return;
    }

    LOG_INFO_TAG("FS", "Contents of %s:", path);

    for (;;)
    {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == '\0')
        {
            break;
        }

        /* Skip hidden/system files starting with . */
        if (fno.fname[0] == '.')
        {
            continue;
        }

        if (fno.fattrib & AM_DIR)
        {
            LOG_INFO_TAG("FS", "  [DIR]  %s/", fno.fname);
            dir_count++;
        }
        else
        {
            format_size(fno.fsize, size_str, sizeof(size_str));
            LOG_INFO_TAG("FS", "  %8s  %s", size_str, fno.fname);
            file_count++;
        }
    }

    f_closedir(&dir);

    LOG_INFO_TAG("FS", "  %d files, %d directories", file_count, dir_count);
}

/**
  * @brief  Filesystem reader thread entry function.
  */
static VOID fs_reader_thread_entry(ULONG thread_input)
{
    FRESULT res;

    TX_PARAMETER_NOT_USED(thread_input);

    /* Wait for SD card to be ready */
    while (!SDMMC1_IsInitialized())
    {
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND);
    }

    /* Mount the filesystem */
    res = f_mount(&SDFatFs, "", 1);  /* 1 = mount immediately */
    if (res != FR_OK)
    {
        LOG_ERROR_TAG("FS", "Mount failed: %s", fs_result_str(res));
        return;
    }

    fs_mounted = 1;

    /* Detect and log filesystem type */
    {
        const char *fs_type;
        switch (SDFatFs.fs_type)
        {
            case FS_FAT12: fs_type = "FAT12"; break;
            case FS_FAT16: fs_type = "FAT16"; break;
            case FS_FAT32: fs_type = "FAT32"; break;
            case FS_EXFAT: fs_type = "exFAT"; break;
            default:       fs_type = "Unknown"; break;
        }
        LOG_INFO_TAG("FS", "Mounted %s filesystem", fs_type);
    }

    /* List root directory */
    fs_list_directory("/");
}
