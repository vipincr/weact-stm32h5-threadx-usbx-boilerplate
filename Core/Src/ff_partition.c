/**
  ******************************************************************************
  * @file    ff_partition.c
  * @brief   FatFs partition table for multi-partition support
  ******************************************************************************
  * This file defines the VolToPart table required when FF_MULTI_PARTITION = 1.
  * It maps FatFs logical volumes to physical drive partitions.
  ******************************************************************************
  */

#include "ff.h"

#if FF_MULTI_PARTITION

/**
  * @brief  Volume to partition mapping table.
  *
  * Format: {physical_drive, partition_number}
  * - physical_drive: 0 = SD card (only one physical drive)
  * - partition_number: 1 = first partition, 2 = second, etc.
  *                     0 = auto-detect (scan MBR/GPT)
  *
  * For GPT-partitioned SD cards (common on Mac), partition 1 typically
  * starts at sector 2048 and contains the exFAT filesystem.
  */
PARTITION VolToPart[FF_VOLUMES] = {
    {0, 1}   /* Volume 0: Physical drive 0, Partition 1 */
};

#endif /* FF_MULTI_PARTITION */
