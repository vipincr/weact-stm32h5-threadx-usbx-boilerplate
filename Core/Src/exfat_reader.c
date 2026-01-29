/**
  ******************************************************************************
  * @file    exfat_reader.c
  * @brief   Lightweight exFAT filesystem reader implementation
  ******************************************************************************
  * @attention
  *
  * This is a read-only exFAT library designed for embedded systems.
  * It provides basic file reading capabilities from exFAT formatted SD cards.
  *
  ******************************************************************************
  */

#include "exfat_reader.h"
#include "sdmmc.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * exFAT On-Disk Structures
 * ============================================================================ */

/* exFAT Boot Sector (Main and Backup) */
#pragma pack(push, 1)
typedef struct {
  uint8_t  jump_boot[3];           /* Jump instruction */
  char     fs_name[8];             /* "EXFAT   " */
  uint8_t  zero[53];               /* Must be zero */
  uint64_t partition_offset;       /* Partition offset in sectors */
  uint64_t volume_length;          /* Volume length in sectors */
  uint32_t fat_offset;             /* FAT offset in sectors */
  uint32_t fat_length;             /* FAT length in sectors */
  uint32_t cluster_heap_offset;    /* Cluster heap offset in sectors */
  uint32_t cluster_count;          /* Total clusters */
  uint32_t first_cluster_of_root;  /* Root directory start cluster */
  uint32_t volume_serial;          /* Volume serial number */
  uint16_t fs_revision;            /* Filesystem revision */
  uint16_t volume_flags;           /* Volume flags */
  uint8_t  bytes_per_sector_shift; /* Log2 of bytes per sector */
  uint8_t  sectors_per_cluster_shift; /* Log2 of sectors per cluster */
  uint8_t  number_of_fats;         /* Number of FATs (1 or 2) */
  uint8_t  drive_select;           /* Drive select */
  uint8_t  percent_in_use;         /* Percent of clusters in use */
  uint8_t  reserved[7];            /* Reserved */
  uint8_t  boot_code[390];         /* Boot code */
  uint16_t boot_signature;         /* 0xAA55 */
} ExFAT_BootSector;

/* exFAT Directory Entry Types */
#define EXFAT_ENTRY_END          0x00  /* End of directory */
#define EXFAT_ENTRY_BITMAP       0x81  /* Allocation bitmap */
#define EXFAT_ENTRY_UPCASE       0x82  /* Up-case table */
#define EXFAT_ENTRY_VOLUME       0x83  /* Volume label */
#define EXFAT_ENTRY_FILE         0x85  /* File directory entry */
#define EXFAT_ENTRY_STREAM       0xC0  /* Stream extension entry */
#define EXFAT_ENTRY_NAME         0xC1  /* File name entry */

/* File Directory Entry (32 bytes) */
typedef struct {
  uint8_t  entry_type;             /* 0x85 for file entry */
  uint8_t  secondary_count;        /* Number of secondary entries */
  uint16_t set_checksum;           /* Checksum of entry set */
  uint16_t file_attributes;        /* File attributes */
  uint16_t reserved1;
  uint32_t create_timestamp;       /* Creation time */
  uint32_t modify_timestamp;       /* Modification time */
  uint32_t access_timestamp;       /* Access time */
  uint8_t  create_10ms;            /* Creation 10ms increment */
  uint8_t  modify_10ms;            /* Modification 10ms increment */
  uint8_t  create_utc_offset;      /* Creation UTC offset */
  uint8_t  modify_utc_offset;      /* Modification UTC offset */
  uint8_t  access_utc_offset;      /* Access UTC offset */
  uint8_t  reserved2[7];
} ExFAT_FileEntry;

/* Stream Extension Entry (32 bytes) */
typedef struct {
  uint8_t  entry_type;             /* 0xC0 for stream entry */
  uint8_t  general_secondary_flags;
  uint8_t  reserved1;
  uint8_t  name_length;            /* File name length in characters */
  uint16_t name_hash;              /* Hash of up-cased file name */
  uint16_t reserved2;
  uint64_t valid_data_length;      /* Valid data length */
  uint32_t reserved3;
  uint32_t first_cluster;          /* First cluster */
  uint64_t data_length;            /* Data length */
} ExFAT_StreamEntry;

/* File Name Entry (32 bytes) */
typedef struct {
  uint8_t  entry_type;             /* 0xC1 for name entry */
  uint8_t  general_secondary_flags;
  uint16_t file_name[15];          /* Part of file name (UCS-2) */
} ExFAT_NameEntry;

/* Volume Label Entry (32 bytes) */
typedef struct {
  uint8_t  entry_type;             /* 0x83 for volume label */
  uint8_t  character_count;        /* Number of characters in label */
  uint16_t volume_label[11];       /* Volume label (UCS-2) */
  uint8_t  reserved[8];
} ExFAT_VolumeLabelEntry;

#pragma pack(pop)

/* ============================================================================
 * Internal State
 * ============================================================================ */

/* exFAT cluster values */
#define EXFAT_CLUSTER_FREE       0x00000000
#define EXFAT_CLUSTER_BAD        0xFFFFFFF7
#define EXFAT_CLUSTER_END        0xFFFFFFFF
#define EXFAT_FIRST_DATA_CLUSTER 2

/* Internal filesystem state */
typedef struct {
  uint8_t  initialized;
  uint32_t bytes_per_sector;
  uint32_t sectors_per_cluster;
  uint32_t bytes_per_cluster;
  uint64_t fat_sector;              /* First sector of FAT */
  uint32_t fat_length;              /* FAT length in sectors */
  uint64_t cluster_heap_sector;     /* First sector of data clusters */
  uint32_t cluster_count;           /* Total clusters */
  uint32_t root_cluster;            /* Root directory cluster */
  char     volume_label[12];        /* Volume label */
  uint64_t volume_length;           /* Volume length in sectors */
} ExFAT_State;

static ExFAT_State fs_state;

/* Sector buffer - use static buffer to avoid stack issues */
#define SECTOR_BUFFER_SIZE  512
static uint8_t sector_buffer[SECTOR_BUFFER_SIZE];

/* ============================================================================
 * Low-Level SD Card Access
 * ============================================================================ */

/**
  * @brief  Read sectors from SD card
  */
static ExFAT_Result read_sectors(uint64_t sector, uint32_t count, uint8_t *buffer)
{
  if (!SDMMC1_IsInitialized()) {
    return EXFAT_ERR_NO_MEDIA;
  }

  if (HAL_SD_ReadBlocks(&hsd1, buffer, (uint32_t)sector, count, 1000) != HAL_OK) {
    return EXFAT_ERR_READ;
  }

  /* Wait for card to be ready */
  uint32_t start = HAL_GetTick();
  while (HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) {
    if ((HAL_GetTick() - start) > 1000) {
      return EXFAT_ERR_READ;
    }
  }

  return EXFAT_OK;
}

/**
  * @brief  Get sector number for a cluster
  */
static uint64_t cluster_to_sector(uint32_t cluster)
{
  if (cluster < EXFAT_FIRST_DATA_CLUSTER) {
    return 0;
  }
  return fs_state.cluster_heap_sector + 
         (uint64_t)(cluster - EXFAT_FIRST_DATA_CLUSTER) * fs_state.sectors_per_cluster;
}

/**
  * @brief  Get next cluster from FAT
  */
static ExFAT_Result get_next_cluster(uint32_t current, uint32_t *next)
{
  if (current < EXFAT_FIRST_DATA_CLUSTER || current >= (fs_state.cluster_count + EXFAT_FIRST_DATA_CLUSTER)) {
    *next = EXFAT_CLUSTER_END;
    return EXFAT_OK;
  }

  /* Calculate FAT entry location */
  uint32_t fat_offset = current * 4;  /* 4 bytes per FAT entry in exFAT */
  uint64_t fat_sector = fs_state.fat_sector + (fat_offset / fs_state.bytes_per_sector);
  uint32_t entry_offset = fat_offset % fs_state.bytes_per_sector;

  /* Read FAT sector */
  ExFAT_Result res = read_sectors(fat_sector, 1, sector_buffer);
  if (res != EXFAT_OK) {
    return res;
  }

  /* Get FAT entry */
  *next = *(uint32_t *)(sector_buffer + entry_offset);

  return EXFAT_OK;
}

/* ============================================================================
 * Name Handling
 * ============================================================================ */

/**
  * @brief  Convert UCS-2 to UTF-8
  */
static void ucs2_to_utf8(const uint16_t *ucs2, int len, char *utf8, int max_len)
{
  int j = 0;
  for (int i = 0; i < len && j < max_len - 1; i++) {
    uint16_t c = ucs2[i];
    if (c == 0) break;
    
    if (c < 0x80) {
      utf8[j++] = (char)c;
    } else if (c < 0x800) {
      if (j + 2 > max_len - 1) break;
      utf8[j++] = 0xC0 | (c >> 6);
      utf8[j++] = 0x80 | (c & 0x3F);
    } else {
      if (j + 3 > max_len - 1) break;
      utf8[j++] = 0xE0 | (c >> 12);
      utf8[j++] = 0x80 | ((c >> 6) & 0x3F);
      utf8[j++] = 0x80 | (c & 0x3F);
    }
  }
  utf8[j] = '\0';
}

/**
  * @brief  Case-insensitive string compare for ASCII
  */
static int strcasecmp_ascii(const char *s1, const char *s2)
{
  while (*s1 && *s2) {
    char c1 = *s1;
    char c2 = *s2;
    /* Convert to lowercase for ASCII letters */
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
    if (c1 != c2) return c1 - c2;
    s1++;
    s2++;
  }
  return *s1 - *s2;
}

/* ============================================================================
 * Directory Parsing
 * ============================================================================ */

/**
  * @brief  Parse a file entry set from directory
  * @note   Entry buffer must be at least 64 bytes for file+stream entries
  */
static ExFAT_Result parse_file_entry(const uint8_t *entry_buffer, int entry_count,
                                      ExFAT_FileInfo *info)
{
  if (entry_count < 2) {
    return EXFAT_ERR_INTERNAL;
  }

  const ExFAT_FileEntry *file = (const ExFAT_FileEntry *)entry_buffer;
  const ExFAT_StreamEntry *stream = (const ExFAT_StreamEntry *)(entry_buffer + 32);

  if ((file->entry_type & 0x7F) != 0x05 || (stream->entry_type & 0x7F) != 0x40) {
    return EXFAT_ERR_INTERNAL;
  }

  /* Fill file info */
  info->attr = file->file_attributes & 0xFF;
  info->start_cluster = stream->first_cluster;
  info->size = stream->data_length;

  /* Parse timestamps (DOS format) */
  info->create_date = (file->create_timestamp >> 16) & 0xFFFF;
  info->create_time = file->create_timestamp & 0xFFFF;
  info->modify_date = (file->modify_timestamp >> 16) & 0xFFFF;
  info->modify_time = file->modify_timestamp & 0xFFFF;

  /* Collect file name from name entries */
  int name_len = stream->name_length;
  if (name_len > EXFAT_MAX_NAME) name_len = EXFAT_MAX_NAME;

  uint16_t name_ucs2[EXFAT_MAX_NAME + 1];
  int name_pos = 0;
  int secondary_count = file->secondary_count;

  for (int i = 2; i <= secondary_count && name_pos < name_len; i++) {
    const ExFAT_NameEntry *name_entry = (const ExFAT_NameEntry *)(entry_buffer + i * 32);
    if ((name_entry->entry_type & 0x7F) != 0x41) continue;  /* Not a name entry */

    for (int j = 0; j < 15 && name_pos < name_len; j++) {
      name_ucs2[name_pos++] = name_entry->file_name[j];
    }
  }
  name_ucs2[name_pos] = 0;

  ucs2_to_utf8(name_ucs2, name_pos, info->name, sizeof(info->name));

  return EXFAT_OK;
}

/**
  * @brief  Find entry in directory by name
  */
static ExFAT_Result find_in_directory(uint32_t dir_cluster, const char *name,
                                       ExFAT_FileInfo *info)
{
  uint32_t cluster = dir_cluster;
  uint8_t entry_set[32 * 20];  /* Buffer for entry set (up to 20 entries) */
  int entry_set_count = 0;
  int collecting_set = 0;

  while (cluster >= EXFAT_FIRST_DATA_CLUSTER && cluster < EXFAT_CLUSTER_END) {
    uint64_t sector = cluster_to_sector(cluster);

    for (uint32_t s = 0; s < fs_state.sectors_per_cluster; s++) {
      ExFAT_Result res = read_sectors(sector + s, 1, sector_buffer);
      if (res != EXFAT_OK) return res;

      for (int e = 0; e < (int)(fs_state.bytes_per_sector / 32); e++) {
        uint8_t *entry = sector_buffer + e * 32;
        uint8_t entry_type = entry[0];

        if (entry_type == EXFAT_ENTRY_END) {
          /* End of directory */
          return EXFAT_ERR_NOT_FOUND;
        }

        if ((entry_type & 0x80) == 0) {
          /* Deleted or unused entry */
          collecting_set = 0;
          entry_set_count = 0;
          continue;
        }

        if (entry_type == EXFAT_ENTRY_FILE) {
          /* Start of new file entry set */
          collecting_set = 1;
          entry_set_count = 0;
          memcpy(entry_set, entry, 32);
          entry_set_count = 1;
        } else if (collecting_set) {
          if (entry_set_count < 20) {
            memcpy(entry_set + entry_set_count * 32, entry, 32);
            entry_set_count++;
          }

          /* Check if we have the complete set */
          ExFAT_FileEntry *file = (ExFAT_FileEntry *)entry_set;
          if (entry_set_count == file->secondary_count + 1) {
            /* Parse and check name */
            ExFAT_FileInfo temp_info;
            if (parse_file_entry(entry_set, entry_set_count, &temp_info) == EXFAT_OK) {
              if (strcasecmp_ascii(temp_info.name, name) == 0) {
                *info = temp_info;
                return EXFAT_OK;
              }
            }
            collecting_set = 0;
            entry_set_count = 0;
          }
        }
      }
    }

    /* Get next cluster */
    ExFAT_Result res = get_next_cluster(cluster, &cluster);
    if (res != EXFAT_OK) return res;
  }

  return EXFAT_ERR_NOT_FOUND;
}

/**
  * @brief  Navigate path to find entry
  */
static ExFAT_Result navigate_path(const char *path, ExFAT_FileInfo *info, uint32_t *parent_cluster)
{
  if (path == NULL || path[0] == '\0') {
    return EXFAT_ERR_INVALID_ARG;
  }

  /* Skip leading slash */
  if (path[0] == '/') path++;

  /* Handle root directory */
  if (path[0] == '\0') {
    /* Return root directory info */
    if (info != NULL) {
      memset(info, 0, sizeof(*info));
      info->name[0] = '/';
      info->name[1] = '\0';
      info->attr = EXFAT_ATTR_DIRECTORY;
      info->start_cluster = fs_state.root_cluster;
    }
    if (parent_cluster != NULL) {
      *parent_cluster = fs_state.root_cluster;
    }
    return EXFAT_OK;
  }

  uint32_t current_cluster = fs_state.root_cluster;
  char component[EXFAT_MAX_NAME + 1];
  const char *p = path;
  ExFAT_FileInfo current_info;

  while (*p) {
    /* Extract path component */
    const char *start = p;
    while (*p && *p != '/') p++;
    int len = p - start;
    if (len > EXFAT_MAX_NAME) len = EXFAT_MAX_NAME;
    memcpy(component, start, len);
    component[len] = '\0';

    /* Skip trailing slash */
    if (*p == '/') p++;

    /* Find in current directory */
    ExFAT_Result res = find_in_directory(current_cluster, component, &current_info);
    if (res != EXFAT_OK) return res;

    /* If more path remains, this must be a directory */
    if (*p && !(current_info.attr & EXFAT_ATTR_DIRECTORY)) {
      return EXFAT_ERR_NOT_DIR;
    }

    if (parent_cluster != NULL) {
      *parent_cluster = current_cluster;
    }

    current_cluster = current_info.start_cluster;
  }

  if (info != NULL) {
    *info = current_info;
  }

  return EXFAT_OK;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

ExFAT_Result ExFAT_Init(void)
{
  if (!SDMMC1_IsInitialized()) {
    LOG_ERROR_TAG("EXFAT", "SD card not initialized");
    return EXFAT_ERR_NO_MEDIA;
  }

  memset(&fs_state, 0, sizeof(fs_state));

  /* Read boot sector */
  ExFAT_Result res = read_sectors(0, 1, sector_buffer);
  if (res != EXFAT_OK) {
    LOG_ERROR_TAG("EXFAT", "Failed to read boot sector");
    return res;
  }

  ExFAT_BootSector *boot = (ExFAT_BootSector *)sector_buffer;

  /* Verify exFAT signature */
  if (memcmp(boot->fs_name, "EXFAT   ", 8) != 0) {
    LOG_ERROR_TAG("EXFAT", "Not an exFAT filesystem");
    return EXFAT_ERR_NOT_EXFAT;
  }

  if (boot->boot_signature != 0xAA55) {
    LOG_ERROR_TAG("EXFAT", "Invalid boot signature");
    return EXFAT_ERR_NOT_EXFAT;
  }

  /* Parse boot sector parameters */
  fs_state.bytes_per_sector = 1 << boot->bytes_per_sector_shift;
  fs_state.sectors_per_cluster = 1 << boot->sectors_per_cluster_shift;
  fs_state.bytes_per_cluster = fs_state.bytes_per_sector * fs_state.sectors_per_cluster;
  fs_state.fat_sector = boot->fat_offset;
  fs_state.fat_length = boot->fat_length;
  fs_state.cluster_heap_sector = boot->cluster_heap_offset;
  fs_state.cluster_count = boot->cluster_count;
  fs_state.root_cluster = boot->first_cluster_of_root;
  fs_state.volume_length = boot->volume_length;

  /* Read volume label from root directory */
  fs_state.volume_label[0] = '\0';
  uint64_t root_sector = cluster_to_sector(fs_state.root_cluster);
  if (read_sectors(root_sector, 1, sector_buffer) == EXFAT_OK) {
    for (int e = 0; e < (int)(fs_state.bytes_per_sector / 32); e++) {
      uint8_t *entry = sector_buffer + e * 32;
      if (entry[0] == EXFAT_ENTRY_VOLUME) {
        ExFAT_VolumeLabelEntry *vol = (ExFAT_VolumeLabelEntry *)entry;
        ucs2_to_utf8(vol->volume_label, vol->character_count, fs_state.volume_label, 12);
        break;
      }
      if (entry[0] == EXFAT_ENTRY_END) break;
    }
  }

  fs_state.initialized = 1;

  LOG_INFO_TAG("EXFAT", "Initialized: %lu clusters, %lu bytes/cluster, label='%s'",
               (unsigned long)fs_state.cluster_count,
               (unsigned long)fs_state.bytes_per_cluster,
               fs_state.volume_label);

  return EXFAT_OK;
}

ExFAT_Result ExFAT_DeInit(void)
{
  memset(&fs_state, 0, sizeof(fs_state));
  return EXFAT_OK;
}

int ExFAT_IsInitialized(void)
{
  return fs_state.initialized ? 1 : 0;
}

ExFAT_Result ExFAT_GetInfo(ExFAT_FSInfo *info)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (info == NULL) return EXFAT_ERR_INVALID_ARG;

  info->total_size = fs_state.volume_length * fs_state.bytes_per_sector;
  info->free_size = 0;  /* Would require parsing allocation bitmap */
  info->cluster_count = fs_state.cluster_count;
  info->sectors_per_cluster = fs_state.sectors_per_cluster;
  info->bytes_per_sector = fs_state.bytes_per_sector;
  memcpy(info->volume_label, fs_state.volume_label, sizeof(info->volume_label));

  return EXFAT_OK;
}

ExFAT_Result ExFAT_FileOpen(const char *path, ExFAT_File *file)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (path == NULL || file == NULL) return EXFAT_ERR_INVALID_ARG;

  memset(file, 0, sizeof(*file));

  ExFAT_FileInfo info;
  ExFAT_Result res = navigate_path(path, &info, NULL);
  if (res != EXFAT_OK) return res;

  if (info.attr & EXFAT_ATTR_DIRECTORY) {
    return EXFAT_ERR_IS_DIR;
  }

  file->is_open = 1;
  file->is_dir = 0;
  file->start_cluster = info.start_cluster;
  file->current_cluster = info.start_cluster;
  file->size = info.size;
  file->position = 0;
  file->cluster_offset = 0;

  return EXFAT_OK;
}

ExFAT_Result ExFAT_FileClose(ExFAT_File *file)
{
  if (file == NULL) return EXFAT_ERR_INVALID_ARG;
  memset(file, 0, sizeof(*file));
  return EXFAT_OK;
}

ExFAT_Result ExFAT_FileRead(ExFAT_File *file, void *buffer, size_t size, size_t *bytes_read)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (file == NULL || buffer == NULL || bytes_read == NULL) return EXFAT_ERR_INVALID_ARG;
  if (!file->is_open) return EXFAT_ERR_INVALID_ARG;

  *bytes_read = 0;

  /* Check EOF */
  if (file->position >= file->size) {
    return EXFAT_ERR_EOF;
  }

  /* Limit read size to remaining file */
  if (file->position + size > file->size) {
    size = (size_t)(file->size - file->position);
  }

  uint8_t *dest = (uint8_t *)buffer;
  size_t remaining = size;

  while (remaining > 0 && file->current_cluster >= EXFAT_FIRST_DATA_CLUSTER && 
         file->current_cluster < EXFAT_CLUSTER_END) {
    
    /* Calculate read position within cluster */
    uint64_t cluster_sector = cluster_to_sector(file->current_cluster);
    uint32_t sector_in_cluster = file->cluster_offset / fs_state.bytes_per_sector;
    uint32_t offset_in_sector = file->cluster_offset % fs_state.bytes_per_sector;

    /* Read sector */
    ExFAT_Result res = read_sectors(cluster_sector + sector_in_cluster, 1, sector_buffer);
    if (res != EXFAT_OK) return res;

    /* Copy data from sector */
    uint32_t bytes_in_sector = fs_state.bytes_per_sector - offset_in_sector;
    if (bytes_in_sector > remaining) bytes_in_sector = remaining;

    memcpy(dest, sector_buffer + offset_in_sector, bytes_in_sector);
    dest += bytes_in_sector;
    remaining -= bytes_in_sector;
    *bytes_read += bytes_in_sector;
    file->position += bytes_in_sector;
    file->cluster_offset += bytes_in_sector;

    /* Move to next cluster if needed */
    if (file->cluster_offset >= fs_state.bytes_per_cluster) {
      res = get_next_cluster(file->current_cluster, &file->current_cluster);
      if (res != EXFAT_OK) return res;
      file->cluster_offset = 0;
    }
  }

  return (*bytes_read > 0) ? EXFAT_OK : EXFAT_ERR_EOF;
}

ExFAT_Result ExFAT_FileSeek(ExFAT_File *file, int64_t offset, int origin)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (file == NULL || !file->is_open) return EXFAT_ERR_INVALID_ARG;

  int64_t new_pos;
  switch (origin) {
    case 0: new_pos = offset; break;
    case 1: new_pos = (int64_t)file->position + offset; break;
    case 2: new_pos = (int64_t)file->size + offset; break;
    default: return EXFAT_ERR_INVALID_ARG;
  }

  if (new_pos < 0) new_pos = 0;
  if (new_pos > (int64_t)file->size) new_pos = (int64_t)file->size;

  /* Need to recalculate current cluster */
  uint64_t cluster_index = (uint64_t)new_pos / fs_state.bytes_per_cluster;
  uint32_t cluster = file->start_cluster;

  for (uint64_t i = 0; i < cluster_index && cluster >= EXFAT_FIRST_DATA_CLUSTER && 
       cluster < EXFAT_CLUSTER_END; i++) {
    ExFAT_Result res = get_next_cluster(cluster, &cluster);
    if (res != EXFAT_OK) return res;
  }

  file->position = (uint64_t)new_pos;
  file->current_cluster = cluster;
  file->cluster_offset = (uint32_t)((uint64_t)new_pos % fs_state.bytes_per_cluster);

  return EXFAT_OK;
}

int64_t ExFAT_FileTell(const ExFAT_File *file)
{
  if (file == NULL || !file->is_open) return -1;
  return (int64_t)file->position;
}

uint64_t ExFAT_FileSize(const ExFAT_File *file)
{
  if (file == NULL || !file->is_open) return 0;
  return file->size;
}

int ExFAT_FileEOF(const ExFAT_File *file)
{
  if (file == NULL || !file->is_open) return 1;
  return (file->position >= file->size) ? 1 : 0;
}

ExFAT_Result ExFAT_DirOpen(const char *path, ExFAT_Dir *dir)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (path == NULL || dir == NULL) return EXFAT_ERR_INVALID_ARG;

  memset(dir, 0, sizeof(*dir));

  ExFAT_FileInfo info;
  ExFAT_Result res = navigate_path(path, &info, NULL);
  if (res != EXFAT_OK) return res;

  if (!(info.attr & EXFAT_ATTR_DIRECTORY)) {
    return EXFAT_ERR_NOT_DIR;
  }

  dir->is_open = 1;
  dir->start_cluster = info.start_cluster;
  dir->current_cluster = info.start_cluster;
  dir->entry_offset = 0;

  return EXFAT_OK;
}

ExFAT_Result ExFAT_DirClose(ExFAT_Dir *dir)
{
  if (dir == NULL) return EXFAT_ERR_INVALID_ARG;
  memset(dir, 0, sizeof(*dir));
  return EXFAT_OK;
}

ExFAT_Result ExFAT_DirRead(ExFAT_Dir *dir, ExFAT_FileInfo *info)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (dir == NULL || info == NULL || !dir->is_open) return EXFAT_ERR_INVALID_ARG;

  uint8_t entry_set[32 * 20];
  int entry_set_count = 0;
  int collecting_set = 0;

  while (dir->current_cluster >= EXFAT_FIRST_DATA_CLUSTER && 
         dir->current_cluster < EXFAT_CLUSTER_END) {
    
    uint64_t cluster_sector = cluster_to_sector(dir->current_cluster);
    uint32_t entries_per_cluster = fs_state.bytes_per_cluster / 32;

    while (dir->entry_offset < entries_per_cluster) {
      uint32_t sector_in_cluster = (dir->entry_offset * 32) / fs_state.bytes_per_sector;
      uint32_t entry_in_sector = ((dir->entry_offset * 32) % fs_state.bytes_per_sector) / 32;

      ExFAT_Result res = read_sectors(cluster_sector + sector_in_cluster, 1, sector_buffer);
      if (res != EXFAT_OK) return res;

      uint8_t *entry = sector_buffer + entry_in_sector * 32;
      uint8_t entry_type = entry[0];

      dir->entry_offset++;

      if (entry_type == EXFAT_ENTRY_END) {
        return EXFAT_ERR_NOT_FOUND;
      }

      if ((entry_type & 0x80) == 0) {
        collecting_set = 0;
        entry_set_count = 0;
        continue;
      }

      if (entry_type == EXFAT_ENTRY_FILE) {
        collecting_set = 1;
        entry_set_count = 0;
        memcpy(entry_set, entry, 32);
        entry_set_count = 1;
      } else if (collecting_set && entry_set_count < 20) {
        memcpy(entry_set + entry_set_count * 32, entry, 32);
        entry_set_count++;

        ExFAT_FileEntry *file = (ExFAT_FileEntry *)entry_set;
        if (entry_set_count == file->secondary_count + 1) {
          res = parse_file_entry(entry_set, entry_set_count, info);
          if (res == EXFAT_OK) {
            return EXFAT_OK;
          }
          collecting_set = 0;
          entry_set_count = 0;
        }
      }
    }

    /* Move to next cluster */
    ExFAT_Result res = get_next_cluster(dir->current_cluster, &dir->current_cluster);
    if (res != EXFAT_OK) return res;
    dir->entry_offset = 0;
  }

  return EXFAT_ERR_NOT_FOUND;
}

ExFAT_Result ExFAT_DirRewind(ExFAT_Dir *dir)
{
  if (dir == NULL || !dir->is_open) return EXFAT_ERR_INVALID_ARG;
  dir->current_cluster = dir->start_cluster;
  dir->entry_offset = 0;
  return EXFAT_OK;
}

ExFAT_Result ExFAT_Stat(const char *path, ExFAT_FileInfo *info)
{
  if (!fs_state.initialized) return EXFAT_ERR_NOT_INIT;
  if (path == NULL || info == NULL) return EXFAT_ERR_INVALID_ARG;
  return navigate_path(path, info, NULL);
}

int ExFAT_Exists(const char *path)
{
  ExFAT_FileInfo info;
  return (ExFAT_Stat(path, &info) == EXFAT_OK) ? 1 : 0;
}

int ExFAT_IsDirectory(const char *path)
{
  ExFAT_FileInfo info;
  if (ExFAT_Stat(path, &info) != EXFAT_OK) return 0;
  return (info.attr & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
}
