/**
  ******************************************************************************
  * @file    button_handler.c
  * @brief   Button handler thread for USER_BUTTON
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "button_handler.h"
#include "main.h"
#include "logger.h"
#include "time_it.h"
#include "ff.h"
#include "fs_reader.h"
#include "jpeg_processor.h"
#include "sd_adapter.h"
#include "sdmmc.h"
#include "usb.h"
#include "stm32h5xx_hal.h"
#include <string.h>

/* Private defines -----------------------------------------------------------*/
#define BUTTON_THREAD_STACK_SIZE  8192U   /* Large stack for FatFS + JPEG encoding */
#define BUTTON_THREAD_PRIORITY    20U
#define BUTTON_POLL_MS            10U
#define BUTTON_DEBOUNCE_COUNT     5U    /* ~50ms at 10ms poll rate for noise rejection */
#define BUTTON_DOUBLE_CLICK_MS    400U  /* Max time between clicks for double-click */
#define MAX_PATH_LEN              128U
#define MAX_SCAN_DEPTH            4U

/* Private variables ---------------------------------------------------------*/
static TX_THREAD button_thread;
static UCHAR button_thread_stack[BUTTON_THREAD_STACK_SIZE];

/* Private function prototypes -----------------------------------------------*/
static VOID button_thread_entry(ULONG thread_input);
static void scan_and_process_bin_files(const char *path, int depth);
static int check_jpg_exists(const char *bin_path);

/* Public functions ----------------------------------------------------------*/

/**
  * @brief  Initialize and start the button handler thread.
  * @param  byte_pool: Pointer to ThreadX byte pool (unused, static allocation).
  * @retval TX_SUCCESS on success, error code otherwise.
  */
UINT ButtonHandler_Init(TX_BYTE_POOL *byte_pool)
{
  UINT status;
  (void)byte_pool;  /* Static allocation - byte_pool not used */

  status = tx_thread_create(&button_thread,
                            "Button",
                            button_thread_entry,
                            0U,
                            button_thread_stack,
                            BUTTON_THREAD_STACK_SIZE,
                            BUTTON_THREAD_PRIORITY,
                            BUTTON_THREAD_PRIORITY,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);

  return status;
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Check if a corresponding .jpg file exists for a .bin file.
  * @param  bin_path: Path to the .bin file
  * @retval 1 if .jpg exists, 0 if not
  */
static int check_jpg_exists(const char *bin_path)
{
    char jpg_path[MAX_PATH_LEN];
    FILINFO fno;
    size_t len = strlen(bin_path);
    
    /* Must end in .bin */
    if (len < 5 || len >= (MAX_PATH_LEN - 1))
    {
        return 0;
    }
    
    /* Build .jpg path by replacing extension */
    strncpy(jpg_path, bin_path, len - 4);
    jpg_path[len - 4] = '\0';
    strcat(jpg_path, ".jpg");
    
    /* Check if file exists */
    if (f_stat(jpg_path, &fno) == FR_OK)
    {
        return 1;  /* .jpg exists */
    }
    
    return 0;  /* .jpg does not exist */
}

/**
  * @brief  Recursively scan directory for .bin files without .jpg counterparts.
  * @param  path: Directory path to scan
  * @param  depth: Current recursion depth
  */
static void scan_and_process_bin_files(const char *path, int depth)
{
    DIR dir;
    FILINFO fno;
    FRESULT res;
    char full_path[MAX_PATH_LEN];
    uint32_t elapsed_ms;
    int files_found = 0;
    int bins_processed = 0;
    
    if (depth >= MAX_SCAN_DEPTH)
    {
        return;
    }
    
    LOG_DEBUG_TAG("BTN", "Scanning: %s (depth=%d)", path, depth);
    
    /* Check if filesystem is still mounted */
    if (!FS_Reader_IsMounted())
    {
        LOG_ERROR_TAG("BTN", "FS not mounted!");
        return;
    }
    
    res = f_opendir(&dir, path);
    if (res != FR_OK)
    {
        LOG_ERROR_TAG("BTN", "opendir failed: %s (err=%d)", path, (int)res);
        return;
    }
    
    LOG_DEBUG_TAG("BTN", "opendir OK, starting readdir loop");
    
    for (;;)
    {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK)
        {
            LOG_ERROR_TAG("BTN", "readdir error: %d (sd_init=%d)", (int)res, SDMMC1_IsInitialized());
            break;
        }
        if (fno.fname[0] == '\0')
        {
            break;  /* End of directory */
        }
        
        /* Skip hidden files */
        if (fno.fname[0] == '.')
        {
            continue;
        }
        
        files_found++;
        
        /* Build full path */
        size_t path_len = strlen(path);
        if (path_len == 1 && path[0] == '/')
        {
            snprintf(full_path, sizeof(full_path), "/%s", fno.fname);
        }
        else
        {
            snprintf(full_path, sizeof(full_path), "%s/%s", path, fno.fname);
        }
        
        if (fno.fattrib & AM_DIR)
        {
            LOG_DEBUG_TAG("BTN", "Dir: %s", full_path);
            /* Recurse into subdirectory */
            scan_and_process_bin_files(full_path, depth + 1);
        }
        else
        {
            /* Check if it's a .bin file */
            size_t name_len = strlen(fno.fname);
            int is_bin = 0;
            
            if (name_len > 4)
            {
                const char *ext = &fno.fname[name_len - 4];
                if (ext[0] == '.' &&
                    (ext[1] == 'b' || ext[1] == 'B') &&
                    (ext[2] == 'i' || ext[2] == 'I') &&
                    (ext[3] == 'n' || ext[3] == 'N'))
                {
                    is_bin = 1;
                }
            }
            
            if (is_bin)
            {
                LOG_DEBUG_TAG("BTN", "Found .bin: %s", full_path);
                
                /* Check if .jpg exists */
                if (!check_jpg_exists(full_path))
                {
                    LOG_INFO_TAG("BTN", "Processing: %s", fno.fname);
                    bins_processed++;
                    
                    JPEG_Processor_Status_t status;
                    TIME_IT(elapsed_ms, status = JPEG_Processor_ConvertFile(full_path, NULL));
                    
                    if (status == JPEG_PROC_OK)
                    {
                        LOG_INFO_TAG("BTN", "Done: %lu ms, %lu bytes",
                                     (unsigned long)elapsed_ms,
                                     (unsigned long)JPEG_Processor_GetLastOutputSize());
                    }
                    else
                    {
                        LOG_ERROR_TAG("BTN", "Failed: err=%d", (int)status);
                    }
                }
                else
                {
                    LOG_DEBUG_TAG("BTN", "Skipped (jpg exists): %s", fno.fname);
                }
            }
        }
    }
    
    f_closedir(&dir);
    LOG_DEBUG_TAG("BTN", "Dir done: %s (%d files, %d bins)", path, files_found, bins_processed);
}

/**
  * @brief  Handle single click action based on current mode.
  */
static void handle_single_click(void)
{
    SD_Mode_t mode = SD_GetMode();
    
    if (mode == SD_MODE_MSC)
    {
        /* In MSC mode, single click does nothing */
        LOG_DEBUG_TAG("BTN", "Single click ignored (MSC mode)");
        return;
    }
    
    /* FatFS mode - scan and process .bin files */
    if (!JPEG_Processor_IsInitialized())
    {
        LOG_WARN_TAG("BTN", "JPEG processor not ready");
        return;
    }
    
    if (!FS_Reader_IsMounted())
    {
        LOG_WARN_TAG("BTN", "Filesystem not mounted");
        return;
    }
    
    LOG_INFO_TAG("BTN", "Scanning for unprocessed .bin files...");
    
    uint32_t total_ms;
    TIME_IT(total_ms, scan_and_process_bin_files("/", 0));
    
    LOG_INFO_TAG("BTN", "Scan complete (%lu ms)", (unsigned long)total_ms);
}

/**
  * @brief  Handle double click action - toggle between FatFS and MSC modes.
  *
  * This is implemented as a clean finite state machine:
  *
  * States:
  *   SD_MODE_FATFS - FatFS mounted, MSC reports "no media" to host
  *   SD_MODE_MSC   - MSC active, FatFS unmounted, host can access disk
  *
  * Transitions:
  *   FATFS -> MSC:
  *     1. Unmount FatFS
  *     2. If previously ejected, signal media change (UNIT ATTENTION)
  *     3. Set mode to MSC
  *     Always succeeds.
  *
  *   MSC -> FATFS:
  *     Pre-condition: Host must have ejected the disk (SD_IsEjected() == true)
  *     If not ejected: Stay in MSC mode, log error, user must eject first.
  *     If ejected:
  *       1. Set mode to FATFS (MSC starts reporting "no media")
  *       2. Clear ejected flag
  *       3. Mount FatFS
  *       4. If mount fails: Revert to MSC mode
  *
  * Error handling: On any failure, state reverts to previous stable state.
  */
static void handle_double_click(void)
{
    SD_Mode_t current_mode = SD_GetMode();
    
    if (current_mode == SD_MODE_FATFS)
    {
        /*
         * Transition: FATFS -> MSC
         * This always succeeds.
         */
        LOG_INFO_TAG("BTN", "Switching to MSC mode...");
        
        /* Step 1: Unmount FatFS */
        FS_Reader_Unmount();
        
        /* Step 2: Check if we need to signal media change */
        int was_ejected = SD_IsEjected();
        SD_ClearEjected();
        
        if (was_ejected)
        {
            /* Host had ejected previously - signal media is now available.
             * This triggers UNIT ATTENTION on next host poll. */
            SD_SetMediaChanged();
            LOG_DEBUG_TAG("BTN", "Signaling media change to host");
        }
        
        /* Step 3: Set MSC mode - MSC callbacks will now allow SD access */
        SD_SetMode(SD_MODE_MSC);
        
        /* Signal media change so host re-queries the device */
        SD_SetMediaChanged();
        
        LOG_INFO_TAG("BTN", "MSC mode active - disk visible to host");
    }
    else  /* current_mode == SD_MODE_MSC */
    {
        /*
         * Transition: MSC -> FATFS
         * Pre-condition: Host must have ejected the disk.
         */
        
        /* Check pre-condition */
        if (!SD_IsEjected())
        {
            /* Host is still using the disk - cannot switch.
             * Stay in MSC mode, user must eject first. */
            LOG_WARN_TAG("BTN", "Cannot switch: disk not ejected by host");
            LOG_INFO_TAG("BTN", "Please eject disk from Finder, then try again");
            return;  /* Stay in MSC mode */
        }
        
        /* Pre-condition met: host has ejected */
        LOG_INFO_TAG("BTN", "Switching to FatFS mode...");
        
        /* Step 1: Set mode to FATFS (MSC will report "no media") */
        SD_SetMode(SD_MODE_FATFS);
        
        /* Step 2: Clear ejected flag (we're taking over the disk now) */
        SD_ClearEjected();
        
        /* Step 3: Mount FatFS */
        if (FS_Reader_Mount() == 0)
        {
            /* Success */
            LOG_INFO_TAG("BTN", "FatFS mode active - ready for encoding");
        }
        else
        {
            /* Mount failed - revert to MSC mode */
            LOG_ERROR_TAG("BTN", "FatFS mount failed - reverting to MSC mode");
            SD_SetMode(SD_MODE_MSC);
            /* Note: Don't set ejected flag - let user try again */
        }
    }
}

/**
  * @brief  Button handler thread - polls USER_BUTTON with single/double click detection.
  * @param  thread_input: Thread input parameter (unused).
  *
  * Click detection:
  * - Single click: press + release, wait for double-click timeout
  * - Double click: two presses within BUTTON_DOUBLE_CLICK_MS
  */
static VOID button_thread_entry(ULONG thread_input)
{
  TX_PARAMETER_NOT_USED(thread_input);

  GPIO_PinState stable_state = GPIO_PIN_RESET;  /* Assume released initially */
  GPIO_PinState current_state;
  uint32_t consecutive_count = 0U;
  uint32_t last_press_tick = 0U;
  int pending_click = 0;  /* Set when we have one click and waiting for potential second */

  /* Wait for GPIO to stabilize after boot */
  tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / 2U);

  /* Read initial state */
  stable_state = HAL_GPIO_ReadPin(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);
  
  /* Log initial mode */
  LOG_INFO_TAG("BTN", "Button handler ready (FatFS mode)");

  for (;;)
  {
    current_state = HAL_GPIO_ReadPin(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);
    uint32_t now = HAL_GetTick();

    if (current_state == stable_state)
    {
      /* Same as stable state - reset debounce counter */
      consecutive_count = 0U;
      
      /* Check if we have a pending single click that timed out */
      if (pending_click && (now - last_press_tick) > BUTTON_DOUBLE_CLICK_MS)
      {
        pending_click = 0;
        handle_single_click();
      }
    }
    else
    {
      /* Different from stable state - count consecutive readings */
      consecutive_count++;
      if (consecutive_count >= BUTTON_DEBOUNCE_COUNT)
      {
        /* State has been different for long enough - accept transition */
        if ((current_state == GPIO_PIN_SET) && (stable_state == GPIO_PIN_RESET))
        {
          /* Button pressed */
          if (pending_click && (now - last_press_tick) <= BUTTON_DOUBLE_CLICK_MS)
          {
            /* This is a double-click! */
            pending_click = 0;
            handle_double_click();
          }
          else
          {
            /* First click - mark as pending and wait for potential second click */
            pending_click = 1;
            last_press_tick = now;
          }
        }
        /* Note: We only act on press, not release */
        
        stable_state = current_state;
        consecutive_count = 0U;
      }
    }

    /* Poll every 10ms */
    tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / (1000U / BUTTON_POLL_MS));
  }
}
