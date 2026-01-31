/**
  ******************************************************************************
  * @file    fs_reader.c
  * @brief   Filesystem reader - ThreadX wrapper for FatFs with change monitoring
  ******************************************************************************
  * This module provides a ThreadX thread that mounts the SD card's exFAT
  * filesystem, lists contents, and monitors for changes via callbacks.
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "fs_reader.h"
#include "ff.h"
#include "logger.h"
#include "sdmmc.h"
#include "sd_adapter.h"
#include "tx_api.h"
#include <string.h>
#include <stdio.h>

/* Private defines -----------------------------------------------------------*/
#define FS_READER_THREAD_STACK_SIZE   4096U  /* Stack for FatFs + exFAT + recursion */
#define FS_READER_THREAD_PRIORITY     8U     /* Higher than USB (10) to mount before USB enumerates */

/* Filesystem monitoring configuration */
#define FS_MONITOR_MAX_ENTRIES     128U  /* Max total files/dirs to track */
#define FS_MONITOR_POLL_SECONDS    5U    /* Poll interval in seconds */
#define FS_MONITOR_MAX_PATH_LEN    128U  /* Max full path length */
#define FS_MONITOR_MAX_DEPTH       4U    /* Max recursion depth for subdirectories */

/* Private types -------------------------------------------------------------*/

/**
  * @brief  Cached file/directory entry for change detection.
  *         Stores full path for callback notification.
  */
typedef struct {
    char     path[FS_MONITOR_MAX_PATH_LEN];  /* Full path (e.g., "/subdir/file.txt") */
    FSIZE_t  size;                            /* File size (0 for directories) */
    WORD     fdate;                           /* Modification date */
    WORD     ftime;                           /* Modification time */
    BYTE     is_dir;                          /* 1 if directory, 0 if file */
    BYTE     valid;                           /* 1 if entry is in use */
} FS_EntryCache_t;

/**
  * @brief  Snapshot of entire filesystem tree for change detection.
  */
typedef struct {
    FS_EntryCache_t entries[FS_MONITOR_MAX_ENTRIES];
    uint16_t        count;       /* Number of valid entries */
    uint8_t         initialized; /* 1 if snapshot has been taken */
    uint8_t         has_error;   /* 1 if disk error occurred during snapshot */
} FS_Snapshot_t;

/* Private variables ---------------------------------------------------------*/
static TX_THREAD fs_reader_thread;
static UCHAR fs_reader_thread_stack[FS_READER_THREAD_STACK_SIZE];

static FATFS SDFatFs;       /* FatFs filesystem object */
static volatile int fs_mounted = 0;

/* Filesystem snapshot for change detection */
static FS_Snapshot_t fs_snapshot;
static FS_Snapshot_t fs_new_snapshot;  /* Second snapshot for comparison (too large for stack) */

/* User-registered callback for change notifications */
static FS_ChangeCallback_t fs_change_callback = NULL;

/* Private function prototypes -----------------------------------------------*/
static VOID fs_reader_thread_entry(ULONG thread_input);
static void fs_list_directory(const char *path);
static const char* fs_result_str(FRESULT res);
static void fs_take_snapshot_recursive(const char *path, FS_Snapshot_t *snapshot, int depth);
static void fs_detect_changes(FS_Snapshot_t *old_snap, FS_Snapshot_t *new_snap);
static const FS_EntryCache_t* fs_find_entry(const FS_Snapshot_t *snap, const char *path);
static void fs_notify_change(FS_EventType_t event, const char *path);
static void fs_default_change_handler(FS_EventType_t event_type, const char *path);
static void fs_build_path(char *dest, size_t dest_len, const char *dir, const char *name);
static void format_size(FSIZE_t size, char *buf, size_t buf_len);

/* Public functions ----------------------------------------------------------*/

/**
  * @brief  Convert event type to string for logging.
  */
const char* FS_Reader_EventTypeStr(FS_EventType_t event_type)
{
    switch (event_type)
    {
        case FS_EVENT_FILE_CREATED:  return "FILE_CREATED";
        case FS_EVENT_FILE_MODIFIED: return "FILE_MODIFIED";
        case FS_EVENT_FILE_DELETED:  return "FILE_DELETED";
        case FS_EVENT_DIR_CREATED:   return "DIR_CREATED";
        case FS_EVENT_DIR_DELETED:   return "DIR_DELETED";
        default:                      return "UNKNOWN";
    }
}

/**
  * @brief  Initialize the filesystem reader thread.
  */
UINT FS_Reader_Init(TX_BYTE_POOL *byte_pool)
{
    UINT status;
    VOID *stack_ptr;

    (void)byte_pool;  /* Using static allocation */
    stack_ptr = fs_reader_thread_stack;

    /* Set default callback to log changes */
    fs_change_callback = fs_default_change_handler;

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
  * @brief  Register a callback for filesystem change notifications.
  */
void FS_Reader_SetChangeCallback(FS_ChangeCallback_t callback)
{
    fs_change_callback = callback;
}

/**
  * @brief  Check if filesystem is mounted.
  */
int FS_Reader_IsMounted(void)
{
    return fs_mounted;
}

/**
  * @brief  Unmount the filesystem for MSC mode.
  */
int FS_Reader_Unmount(void)
{
    if (!fs_mounted)
    {
        return 0;  /* Already unmounted */
    }
    
    LOG_INFO_TAG("FS", "Unmounting filesystem for MSC mode...");
    
    /* Unmount FatFS */
    f_mount(NULL, "", 0);
    fs_mounted = 0;
    
    /* Clear snapshot since it's no longer valid */
    memset(&fs_snapshot, 0, sizeof(fs_snapshot));
    
    return 0;
}

/**
  * @brief  Mount the filesystem after MSC mode.
  */
int FS_Reader_Mount(void)
{
    FRESULT res;
    
    if (fs_mounted)
    {
        return 0;  /* Already mounted */
    }
    
    if (!SDMMC1_IsInitialized())
    {
        LOG_ERROR_TAG("FS", "SD card not initialized");
        return -1;
    }
    
    LOG_INFO_TAG("FS", "Mounting filesystem...");
    
    res = f_mount(&SDFatFs, "", 1);
    if (res != FR_OK)
    {
        LOG_ERROR_TAG("FS", "Mount failed: %s", fs_result_str(res));
        return -1;
    }
    
    fs_mounted = 1;
    
    /* Take fresh snapshot for monitoring */
    memset(&fs_snapshot, 0, sizeof(fs_snapshot));
    fs_take_snapshot_recursive("/", &fs_snapshot, 0);
    
    LOG_INFO_TAG("FS", "Filesystem mounted (%u entries)", (unsigned)fs_snapshot.count);
    return 0;
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
  * @brief  Default change handler - logs changes using the Logger.
  */
static void fs_default_change_handler(FS_EventType_t event_type, const char *path)
{
    const char *event_str;
    const char *icon;

    switch (event_type)
    {
        case FS_EVENT_FILE_CREATED:
            event_str = "CREATED";
            icon = "+";
            break;
        case FS_EVENT_FILE_MODIFIED:
            event_str = "MODIFIED";
            icon = "*";
            break;
        case FS_EVENT_FILE_DELETED:
            event_str = "DELETED";
            icon = "-";
            break;
        case FS_EVENT_DIR_CREATED:
            event_str = "CREATED";
            icon = "+";
            break;
        case FS_EVENT_DIR_DELETED:
            event_str = "DELETED";
            icon = "-";
            break;
        default:
            event_str = "UNKNOWN";
            icon = "?";
            break;
    }

    /* Log with different format for files vs directories */
    if (event_type == FS_EVENT_DIR_CREATED || event_type == FS_EVENT_DIR_DELETED)
    {
        LOG_INFO_TAG("FS", "[%s%s] %s/", icon, event_str, path);
    }
    else
    {
        LOG_INFO_TAG("FS", "[%s%s] %s", icon, event_str, path);
    }
}

/**
  * @brief  Notify change via callback.
  */
static void fs_notify_change(FS_EventType_t event, const char *path)
{
    if (fs_change_callback != NULL)
    {
        fs_change_callback(event, path);
    }
}

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
  * @brief  List directory contents to logger (non-recursive, for display only).
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
  * @brief  Remount filesystem to clear stale FatFS cache.
  */
int FS_Reader_Remount(void)
{
    FRESULT res;
    
    if (!SDMMC1_IsInitialized())
    {
        return -1;
    }
    
    LOG_DEBUG_TAG("FS", "Remounting filesystem...");
    
    /* Unmount to clear cache */
    f_mount(NULL, "", 0);
    fs_mounted = 0;
    
    /* Small delay for stability */
    tx_thread_sleep(10U);  /* 100ms */
    
    /* Remount */
    res = f_mount(&SDFatFs, "", 1);
    if (res != FR_OK)
    {
        LOG_ERROR_TAG("FS", "Remount failed: %s", fs_result_str(res));
        return -1;
    }
    
    fs_mounted = 1;
    LOG_DEBUG_TAG("FS", "Remount complete");
    return 0;
}

/**
  * @brief  Filesystem reader thread entry function.
  */
static VOID fs_reader_thread_entry(ULONG thread_input)
{
    FRESULT res;

    TX_PARAMETER_NOT_USED(thread_input);

    /* NOTE: SD card is already initialized in main.c before ThreadX starts.
     * Mount FatFS IMMEDIATELY (before USB starts) to avoid race condition.
     * USB device thread has a 200ms delay, so we must mount before that.
     */
    
    if (!SDMMC1_IsInitialized())
    {
        LOG_ERROR_TAG("FS", "SD card not initialized");
        return;
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

    /* List root directory once at boot */
    fs_list_directory("/");
    
    LOG_INFO_TAG("FS", "Filesystem ready");

    /* Thread is done - no continuous monitoring needed.
     * FatFS is accessed only on-demand via button handler.
     * This thread just sleeps forever to keep the mount valid.
     */
    for (;;)
    {
        tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND * 60);  /* Sleep for 1 minute, repeat */
    }
}

/**
  * @brief  Build full path from directory path and filename.
  */
static void fs_build_path(char *dest, size_t dest_len, const char *dir, const char *name)
{
    size_t dir_len = strlen(dir);

    if (dir_len == 1 && dir[0] == '/')
    {
        /* Root directory - just prepend slash */
        snprintf(dest, dest_len, "/%s", name);
    }
    else
    {
        /* Subdirectory - append with slash */
        snprintf(dest, dest_len, "%s/%s", dir, name);
    }
}

/**
  * @brief  Take a snapshot of directory contents recursively.
  */
static void fs_take_snapshot_recursive(const char *path, FS_Snapshot_t *snapshot, int depth)
{
    FRESULT res;
    DIR dir;
    FILINFO fno;
    char full_path[FS_MONITOR_MAX_PATH_LEN];

    /* Limit recursion depth to prevent stack overflow */
    if (depth >= FS_MONITOR_MAX_DEPTH)
    {
        return;
    }

    /* Stop if we already encountered an error */
    if (snapshot->has_error)
    {
        return;
    }

    res = f_opendir(&dir, path);
    if (res != FR_OK)
    {
        if (res == FR_DISK_ERR || res == FR_NOT_READY || res == FR_TIMEOUT)
        {
            snapshot->has_error = 1;
        }
        return;
    }

    for (;;)
    {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK)
        {
            if (res == FR_DISK_ERR || res == FR_NOT_READY || res == FR_TIMEOUT)
            {
                snapshot->has_error = 1;
            }
            break;
        }
        
        if (fno.fname[0] == '\0')
        {
            break;
        }

        /* Skip hidden files */
        if (fno.fname[0] == '.')
        {
            continue;
        }

        /* Check if we have room for more entries */
        if (snapshot->count >= FS_MONITOR_MAX_ENTRIES)
        {
            break;
        }

        /* Build full path */
        fs_build_path(full_path, sizeof(full_path), path, fno.fname);

        /* Add entry to snapshot */
        FS_EntryCache_t *entry = &snapshot->entries[snapshot->count];
        strncpy(entry->path, full_path, FS_MONITOR_MAX_PATH_LEN - 1);
        entry->path[FS_MONITOR_MAX_PATH_LEN - 1] = '\0';
        entry->size = fno.fsize;
        entry->fdate = fno.fdate;
        entry->ftime = fno.ftime;
        entry->is_dir = (fno.fattrib & AM_DIR) ? 1 : 0;
        entry->valid = 1;
        snapshot->count++;

        /* Recurse into subdirectories */
        if (entry->is_dir)
        {
            fs_take_snapshot_recursive(full_path, snapshot, depth + 1);
        }
    }

    f_closedir(&dir);
    snapshot->initialized = 1;
}

/**
  * @brief  Find an entry in a snapshot by full path.
  */
static const FS_EntryCache_t* fs_find_entry(const FS_Snapshot_t *snap, const char *path)
{
    for (uint16_t i = 0; i < snap->count; i++)
    {
        if (snap->entries[i].valid && strcmp(snap->entries[i].path, path) == 0)
        {
            return &snap->entries[i];
        }
    }
    return NULL;
}

/**
  * @brief  Detect and report changes between two snapshots via callback.
  */
static void fs_detect_changes(FS_Snapshot_t *old_snap, FS_Snapshot_t *new_snap)
{
    if (!old_snap->initialized)
    {
        return;  /* No previous snapshot to compare */
    }

    /* Check for new entries and modifications */
    for (uint16_t i = 0; i < new_snap->count; i++)
    {
        const FS_EntryCache_t *new_entry = &new_snap->entries[i];
        if (!new_entry->valid)
        {
            continue;
        }

        const FS_EntryCache_t *old_entry = fs_find_entry(old_snap, new_entry->path);

        if (old_entry == NULL)
        {
            /* Entry is new - created */
            if (new_entry->is_dir)
            {
                fs_notify_change(FS_EVENT_DIR_CREATED, new_entry->path);
            }
            else
            {
                fs_notify_change(FS_EVENT_FILE_CREATED, new_entry->path);
            }
        }
        else
        {
            /* Entry exists - check for modifications (files only) */
            if (!new_entry->is_dir)
            {
                int modified = 0;

                /* Check size change */
                if (new_entry->size != old_entry->size)
                {
                    modified = 1;
                }

                /* Check date/time change */
                if (new_entry->fdate != old_entry->fdate ||
                    new_entry->ftime != old_entry->ftime)
                {
                    modified = 1;
                }

                if (modified)
                {
                    fs_notify_change(FS_EVENT_FILE_MODIFIED, new_entry->path);
                }
            }
        }
    }

    /* Check for deleted entries */
    for (uint16_t i = 0; i < old_snap->count; i++)
    {
        const FS_EntryCache_t *old_entry = &old_snap->entries[i];
        if (!old_entry->valid)
        {
            continue;
        }

        const FS_EntryCache_t *new_entry = fs_find_entry(new_snap, old_entry->path);

        if (new_entry == NULL)
        {
            /* Entry was deleted */
            if (old_entry->is_dir)
            {
                fs_notify_change(FS_EVENT_DIR_DELETED, old_entry->path);
            }
            else
            {
                fs_notify_change(FS_EVENT_FILE_DELETED, old_entry->path);
            }
        }
    }
}
