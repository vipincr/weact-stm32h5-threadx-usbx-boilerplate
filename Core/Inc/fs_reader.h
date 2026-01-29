/**
  ******************************************************************************
  * @file    fs_reader.h
  * @brief   Filesystem reader header - ThreadX wrapper for FatFs with monitoring
  ******************************************************************************
  */
#ifndef FS_READER_H
#define FS_READER_H

#include "tx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Filesystem change event types.
  */
typedef enum {
    FS_EVENT_FILE_CREATED,      /**< A new file was created */
    FS_EVENT_FILE_MODIFIED,     /**< A file was modified (size or timestamp changed) */
    FS_EVENT_FILE_DELETED,      /**< A file was deleted */
    FS_EVENT_DIR_CREATED,       /**< A new directory was created */
    FS_EVENT_DIR_DELETED,       /**< A directory was deleted */
} FS_EventType_t;

/**
  * @brief  Callback function type for filesystem change notifications.
  * @param  event_type  Type of change event
  * @param  path        Full path to the changed file/directory (e.g., "/subdir/file.txt")
  */
typedef void (*FS_ChangeCallback_t)(FS_EventType_t event_type, const char *path);

/**
  * @brief  Initialize the filesystem reader thread.
  *         Creates a ThreadX thread that will mount the SD card, list
  *         the root directory contents, and monitor for changes.
  * @param  byte_pool  Pointer to ThreadX byte pool for stack allocation,
  *                    or TX_NULL to use static allocation.
  * @retval TX_SUCCESS on success, error code otherwise.
  */
UINT FS_Reader_Init(TX_BYTE_POOL *byte_pool);

/**
  * @brief  Register a callback for filesystem change notifications.
  *         The callback will be invoked when files or directories are
  *         created, modified, or deleted.
  * @param  callback  Function to call on change events, or NULL to disable.
  */
void FS_Reader_SetChangeCallback(FS_ChangeCallback_t callback);

/**
  * @brief  Check if filesystem is mounted and ready.
  * @retval 1 if mounted, 0 otherwise.
  */
int FS_Reader_IsMounted(void);

/**
  * @brief  List contents of a directory to the logger.
  * @param  path  Path to directory (e.g., "/" for root, "/subdir")
  * @retval 0 on success, -1 on error.
  */
int FS_Reader_ListDir(const char *path);

/**
  * @brief  Convert event type to string for logging.
  * @param  event_type  The event type
  * @retval String representation of the event type
  */
const char* FS_Reader_EventTypeStr(FS_EventType_t event_type);

#ifdef __cplusplus
}
#endif

#endif /* FS_READER_H */
