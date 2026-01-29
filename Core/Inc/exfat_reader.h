/**
  ******************************************************************************
  * @file    exfat_reader.h
  * @brief   Lightweight exFAT filesystem reader for SD card
  ******************************************************************************
  * @attention
  *
  * This is a read-only exFAT library designed for embedded systems.
  * It provides basic file reading capabilities from exFAT formatted SD cards.
  *
  ******************************************************************************
  */
#ifndef EXFAT_READER_H
#define EXFAT_READER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Result codes */
typedef enum {
  EXFAT_OK = 0,           /* Operation successful */
  EXFAT_ERR_NOT_INIT,     /* Filesystem not initialized */
  EXFAT_ERR_NO_MEDIA,     /* No SD card present */
  EXFAT_ERR_READ,         /* Read error from media */
  EXFAT_ERR_NOT_EXFAT,    /* Not a valid exFAT filesystem */
  EXFAT_ERR_NOT_FOUND,    /* File or directory not found */
  EXFAT_ERR_IS_DIR,       /* Path is a directory, not a file */
  EXFAT_ERR_NOT_DIR,      /* Path is a file, not a directory */
  EXFAT_ERR_INVALID_ARG,  /* Invalid argument */
  EXFAT_ERR_EOF,          /* End of file reached */
  EXFAT_ERR_BUFFER_SMALL, /* Buffer too small */
  EXFAT_ERR_INTERNAL,     /* Internal error */
} ExFAT_Result;

/* File attributes */
#define EXFAT_ATTR_READ_ONLY  0x01
#define EXFAT_ATTR_HIDDEN     0x02
#define EXFAT_ATTR_SYSTEM     0x04
#define EXFAT_ATTR_DIRECTORY  0x10
#define EXFAT_ATTR_ARCHIVE    0x20

/* Maximum path length */
#define EXFAT_MAX_PATH        256
#define EXFAT_MAX_NAME        255

/* File information structure */
typedef struct {
  char name[EXFAT_MAX_NAME + 1];  /* File/directory name (UTF-8) */
  uint64_t size;                   /* File size in bytes */
  uint8_t attr;                    /* File attributes */
  uint32_t start_cluster;          /* Starting cluster */
  uint16_t create_date;            /* Creation date (DOS format) */
  uint16_t create_time;            /* Creation time (DOS format) */
  uint16_t modify_date;            /* Modification date (DOS format) */
  uint16_t modify_time;            /* Modification time (DOS format) */
} ExFAT_FileInfo;

/* File handle for reading */
typedef struct {
  uint8_t is_open;                 /* File is open */
  uint8_t is_dir;                  /* Is directory */
  uint32_t start_cluster;          /* Starting cluster */
  uint32_t current_cluster;        /* Current cluster being read */
  uint64_t size;                   /* Total size */
  uint64_t position;               /* Current read position */
  uint32_t cluster_offset;         /* Offset within current cluster */
} ExFAT_File;

/* Directory handle for enumeration */
typedef struct {
  uint8_t is_open;                 /* Directory is open */
  uint32_t start_cluster;          /* Starting cluster of directory */
  uint32_t current_cluster;        /* Current cluster */
  uint32_t entry_offset;           /* Current entry offset in cluster */
} ExFAT_Dir;

/* Filesystem information */
typedef struct {
  uint64_t total_size;             /* Total filesystem size in bytes */
  uint64_t free_size;              /* Free space in bytes (if available) */
  uint32_t cluster_count;          /* Total cluster count */
  uint32_t sectors_per_cluster;    /* Sectors per cluster */
  uint32_t bytes_per_sector;       /* Bytes per sector */
  char volume_label[12];           /* Volume label */
} ExFAT_FSInfo;

/**
  * @brief  Initialize exFAT filesystem
  * @note   Must be called before any other exFAT functions.
  *         SD card must be initialized with SDMMC1_SafeInit() first.
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_Init(void);

/**
  * @brief  De-initialize exFAT filesystem
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_DeInit(void);

/**
  * @brief  Check if exFAT filesystem is initialized
  * @retval 1 if initialized, 0 otherwise
  */
int ExFAT_IsInitialized(void);

/**
  * @brief  Get filesystem information
  * @param  info: Pointer to ExFAT_FSInfo structure to fill
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_GetInfo(ExFAT_FSInfo *info);

/**
  * @brief  Open a file for reading
  * @param  path: Path to the file (e.g., "/folder/file.txt")
  * @param  file: Pointer to ExFAT_File handle
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_FileOpen(const char *path, ExFAT_File *file);

/**
  * @brief  Close a file
  * @param  file: Pointer to ExFAT_File handle
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_FileClose(ExFAT_File *file);

/**
  * @brief  Read data from a file
  * @param  file: Pointer to ExFAT_File handle
  * @param  buffer: Buffer to store read data
  * @param  size: Number of bytes to read
  * @param  bytes_read: Pointer to store actual bytes read
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_FileRead(ExFAT_File *file, void *buffer, size_t size, size_t *bytes_read);

/**
  * @brief  Seek to a position in the file
  * @param  file: Pointer to ExFAT_File handle
  * @param  offset: Offset from origin
  * @param  origin: 0=beginning, 1=current, 2=end
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_FileSeek(ExFAT_File *file, int64_t offset, int origin);

/**
  * @brief  Get current file position
  * @param  file: Pointer to ExFAT_File handle
  * @retval Current position or -1 on error
  */
int64_t ExFAT_FileTell(const ExFAT_File *file);

/**
  * @brief  Get file size
  * @param  file: Pointer to ExFAT_File handle
  * @retval File size or 0 on error
  */
uint64_t ExFAT_FileSize(const ExFAT_File *file);

/**
  * @brief  Check if end of file is reached
  * @param  file: Pointer to ExFAT_File handle
  * @retval 1 if EOF, 0 otherwise
  */
int ExFAT_FileEOF(const ExFAT_File *file);

/**
  * @brief  Open a directory for enumeration
  * @param  path: Path to the directory (e.g., "/" for root)
  * @param  dir: Pointer to ExFAT_Dir handle
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_DirOpen(const char *path, ExFAT_Dir *dir);

/**
  * @brief  Close a directory
  * @param  dir: Pointer to ExFAT_Dir handle
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_DirClose(ExFAT_Dir *dir);

/**
  * @brief  Read next directory entry
  * @param  dir: Pointer to ExFAT_Dir handle
  * @param  info: Pointer to ExFAT_FileInfo to fill
  * @retval ExFAT_Result (EXFAT_ERR_NOT_FOUND when no more entries)
  */
ExFAT_Result ExFAT_DirRead(ExFAT_Dir *dir, ExFAT_FileInfo *info);

/**
  * @brief  Rewind directory to first entry
  * @param  dir: Pointer to ExFAT_Dir handle
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_DirRewind(ExFAT_Dir *dir);

/**
  * @brief  Get file information by path
  * @param  path: Path to file or directory
  * @param  info: Pointer to ExFAT_FileInfo to fill
  * @retval ExFAT_Result
  */
ExFAT_Result ExFAT_Stat(const char *path, ExFAT_FileInfo *info);

/**
  * @brief  Check if a path exists
  * @param  path: Path to check
  * @retval 1 if exists, 0 otherwise
  */
int ExFAT_Exists(const char *path);

/**
  * @brief  Check if a path is a directory
  * @param  path: Path to check
  * @retval 1 if directory, 0 otherwise
  */
int ExFAT_IsDirectory(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* EXFAT_READER_H */
