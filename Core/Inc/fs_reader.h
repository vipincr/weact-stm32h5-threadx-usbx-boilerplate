/**
  ******************************************************************************
  * @file    fs_reader.h
  * @brief   Filesystem reader header - ThreadX wrapper for FatFs
  ******************************************************************************
  */
#ifndef FS_READER_H
#define FS_READER_H

#include "tx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
  * @brief  Initialize the filesystem reader thread.
  *         Creates a ThreadX thread that will mount the SD card and list
  *         the root directory contents to the CDC logger.
  * @param  byte_pool  Pointer to ThreadX byte pool for stack allocation,
  *                    or TX_NULL to use static allocation.
  * @retval TX_SUCCESS on success, error code otherwise.
  */
UINT FS_Reader_Init(TX_BYTE_POOL *byte_pool);

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

#ifdef __cplusplus
}
#endif

#endif /* FS_READER_H */
