/**
  ******************************************************************************
  * @file    ff_threadx.c
  * @brief   FatFs OS-dependent functions for ThreadX
  ******************************************************************************
  * This file provides the mutex functions required by FatFs when
  * FF_FS_REENTRANT is enabled for thread-safe operation.
  ******************************************************************************
  */

#include "ff.h"
#include "tx_api.h"

#if FF_FS_REENTRANT

/* Mutex for each volume (FF_VOLUMES) */
static TX_MUTEX ff_mutex[FF_VOLUMES];
static UINT ff_mutex_initialized[FF_VOLUMES] = {0};

/*------------------------------------------------------------------------*/
/* Create a Synchronization Object                                        */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount() to create a new mutex for the volume. */

int ff_mutex_create(int vol)
{
    UINT status;
    char name[] = "FatFs0";

    if (vol >= FF_VOLUMES)
    {
        return 0;  /* Invalid volume */
    }

    /* Create unique name for each volume */
    name[5] = (char)('0' + vol);

    status = tx_mutex_create(&ff_mutex[vol], name, TX_NO_INHERIT);
    
    if (status == TX_SUCCESS)
    {
        ff_mutex_initialized[vol] = 1;
        return 1;  /* Success */
    }

    return 0;  /* Failed */
}

/*------------------------------------------------------------------------*/
/* Delete a Synchronization Object                                        */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount() to delete a mutex for the volume. */

void ff_mutex_delete(int vol)
{
    if (vol >= FF_VOLUMES)
    {
        return;
    }

    if (ff_mutex_initialized[vol])
    {
        tx_mutex_delete(&ff_mutex[vol]);
        ff_mutex_initialized[vol] = 0;
    }
}

/*------------------------------------------------------------------------*/
/* Request Grant to Access the Volume                                     */
/*------------------------------------------------------------------------*/
/* This function is called on enter file functions to lock the volume.
/  When a 0 is returned, the file function fails with FR_TIMEOUT. */

int ff_mutex_take(int vol)
{
    UINT status;

    if (vol >= FF_VOLUMES || !ff_mutex_initialized[vol])
    {
        return 0;  /* Invalid */
    }

    status = tx_mutex_get(&ff_mutex[vol], FF_FS_TIMEOUT);
    
    return (status == TX_SUCCESS) ? 1 : 0;
}

/*------------------------------------------------------------------------*/
/* Release Grant to Access the Volume                                     */
/*------------------------------------------------------------------------*/
/* This function is called on leave file functions to unlock the volume. */

void ff_mutex_give(int vol)
{
    if (vol >= FF_VOLUMES || !ff_mutex_initialized[vol])
    {
        return;
    }

    tx_mutex_put(&ff_mutex[vol]);
}

#endif /* FF_FS_REENTRANT */

/*------------------------------------------------------------------------*/
/* Memory Allocation Functions (for FF_USE_LFN == 3)                      */
/*------------------------------------------------------------------------*/
/* Not needed when FF_USE_LFN == 2 (stack allocation) */

#if FF_USE_LFN == 3  /* Dynamic memory allocation on heap */

#include <stdlib.h>

void* ff_memalloc(UINT msize)
{
    return malloc((size_t)msize);
}

void ff_memfree(void* mblock)
{
    free(mblock);
}

#endif
