/*---------------------------------------------------------------------------/
/  FatFs Configuration - STM32H5 + ThreadX + exFAT
/---------------------------------------------------------------------------*/

#ifndef FFCONF_DEF
#define FFCONF_DEF	80286	/* Revision ID */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* Read/write configuration for file operations.
/  Set to 1 for read-only mode. */

#define FF_FS_MINIMIZE	0
/* Full API enabled including f_opendir, f_readdir, f_closedir */

#define FF_USE_FIND		0
/* Disable filtered directory read (f_findfirst, f_findnext) */

#define FF_USE_MKFS		0
/* Disable f_mkfs (not needed for read-only) */

#define FF_USE_FASTSEEK	0
/* Disable fast seek */

#define FF_USE_EXPAND	0
/* Disable f_expand */

#define FF_USE_CHMOD	0
/* Disable chmod/utime */

#define FF_USE_LABEL	0
/* Disable volume label functions */

#define FF_USE_FORWARD	0
/* Disable f_forward */

#define FF_USE_STRFUNC	0
/* Disable string functions (f_gets, f_putc, etc.) */

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* U.S. code page */

#define FF_USE_LFN		2
/* Enable LFN with dynamic working buffer on STACK.
/  Required for exFAT support.
/   0: Disable LFN
/   1: Enable LFN with static buffer (NOT thread-safe)
/   2: Enable LFN with dynamic buffer on STACK
/   3: Enable LFN with dynamic buffer on HEAP */

#define FF_MAX_LFN		255
/* Maximum LFN length (12-255) */

#define FF_LFN_UNICODE	2
/* Unicode in UTF-8 for API strings (TCHAR = char) */

#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
/* File name buffer sizes in FILINFO */

#define FF_FS_RPATH		0
/* Disable relative path support */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES		1
/* Single volume (SD card) */

#define FF_STR_VOLUME_ID	0
/* Disable string volume IDs */

#define FF_MULTI_PARTITION	1
/* Enable multi-partition support.
/  Required for GPT-partitioned SD cards where filesystem starts at sector 2048.
/  Requires VolToPart[] table in ff_partition.c */

#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* Sector size fixed at 512 bytes */

#define FF_LBA64		1
/* Enable 64-bit LBA for exFAT large volume support */

#define FF_MIN_GPT		0x10000000
/* GPT threshold */

#define FF_USE_TRIM		0
/* Disable TRIM */

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/* Normal buffer configuration */

#define FF_FS_EXFAT		1
/* ENABLE exFAT filesystem support! */

#define FF_FS_NORTC		0
/* Use RTC for timestamps. get_fattime() must be implemented. */

#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2026
/* Fallback date if RTC not available */

#define FF_FS_NOFSINFO	0
/* Trust FSINFO */

#define FF_FS_LOCK		0
/* Disable file lock (read-only doesn't need it) */

#define FF_FS_REENTRANT	1
/* Enable re-entrancy for ThreadX thread safety */

#define FF_FS_TIMEOUT	(1000U)
/* Mutex timeout in OS ticks (1 second) */

/*---------------------------------------------------------------------------/
/ Note: ff_mutex_* functions must be implemented in ffsystem.c
/---------------------------------------------------------------------------*/

#endif /* FFCONF_DEF */
