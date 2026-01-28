/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    sdmmc.c
  * @brief   This file provides code for the configuration
  *          of the SDMMC instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "sdmmc.h"

/* USER CODE BEGIN 0 */
#include "logger.h"
/* USER CODE END 0 */

SD_HandleTypeDef hsd1;

/* SDMMC1 init function */

void MX_SDMMC1_SD_Init(void)
{

  /* USER CODE BEGIN SDMMC1_Init 0 */

  /* USER CODE END SDMMC1_Init 0 */

  /* USER CODE BEGIN SDMMC1_Init 1 */

  /* USER CODE END SDMMC1_Init 1 */
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_4B;
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
  hsd1.Init.ClockDiv = 8;
  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SDMMC1_Init 2 */

  /* USER CODE END SDMMC1_Init 2 */

}

void HAL_SD_MspInit(SD_HandleTypeDef* sdHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};
  if(sdHandle->Instance==SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspInit 0 */

  /* USER CODE END SDMMC1_MspInit 0 */

  /** Initializes the peripherals clock
  */
    PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_SDMMC1;
    PeriphClkInitStruct.PLL2.PLL2Source = RCC_PLL2_SOURCE_CSI;
    PeriphClkInitStruct.PLL2.PLL2M = 2;
    PeriphClkInitStruct.PLL2.PLL2N = 124;
    PeriphClkInitStruct.PLL2.PLL2P = 2;
    PeriphClkInitStruct.PLL2.PLL2Q = 2;
    PeriphClkInitStruct.PLL2.PLL2R = 2;
    PeriphClkInitStruct.PLL2.PLL2RGE = RCC_PLL2_VCIRANGE_1;
    PeriphClkInitStruct.PLL2.PLL2VCOSEL = RCC_PLL2_VCORANGE_WIDE;
    PeriphClkInitStruct.PLL2.PLL2FRACN = 0;
    PeriphClkInitStruct.PLL2.PLL2ClockOut = RCC_PLL2_DIVR;
    PeriphClkInitStruct.Sdmmc1ClockSelection = RCC_SDMMC1CLKSOURCE_PLL2R;
    if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
    {
      Error_Handler();
    }

    /* SDMMC1 clock enable */
    __HAL_RCC_SDMMC1_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    /**SDMMC1 GPIO Configuration
    PC8     ------> SDMMC1_D0
    PC9     ------> SDMMC1_D1
    PC10     ------> SDMMC1_D2
    PC11     ------> SDMMC1_D3
    PC12     ------> SDMMC1_CK
    PD2     ------> SDMMC1_CMD
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11
                          |GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF12_SDMMC1;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* USER CODE BEGIN SDMMC1_MspInit 1 */

  /* USER CODE END SDMMC1_MspInit 1 */
  }
}

void HAL_SD_MspDeInit(SD_HandleTypeDef* sdHandle)
{

  if(sdHandle->Instance==SDMMC1)
  {
  /* USER CODE BEGIN SDMMC1_MspDeInit 0 */

  /* USER CODE END SDMMC1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_SDMMC1_CLK_DISABLE();

    /**SDMMC1 GPIO Configuration
    PC8     ------> SDMMC1_D0
    PC9     ------> SDMMC1_D1
    PC10     ------> SDMMC1_D2
    PC11     ------> SDMMC1_D3
    PC12     ------> SDMMC1_CK
    PD2     ------> SDMMC1_CMD
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_8|GPIO_PIN_9|GPIO_PIN_10|GPIO_PIN_11
                          |GPIO_PIN_12);

    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_2);

  /* USER CODE BEGIN SDMMC1_MspDeInit 1 */

  /* USER CODE END SDMMC1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

static uint8_t sd_initialized = 0U;
static uint8_t msc_enabled = 0U;  /* Whether MSC class should be active */
static uint32_t last_card_check_tick = 0U;

/* MSC enable control - called before USB init based on SD detection */
void USBD_MSC_SetEnabled(int enabled)
{
  msc_enabled = (enabled != 0) ? 1U : 0U;
}

int USBD_MSC_IsEnabled(void)
{
  return (msc_enabled != 0U) ? 1 : 0;
}

/**
  * @brief  Reset SD state to allow re-initialization.
  *         Call this when card is removed to allow detecting a new card.
  */
void SDMMC1_ResetState(void)
{
  if (sd_initialized != 0U)
  {
    LOG_INFO_TAG("SD", "SD card removed - resetting state");
    HAL_SD_DeInit(&hsd1);
  }
  sd_initialized = 0U;
}

/**
  * @brief  Check if SD card is currently present and accessible.
  *         This is a quick check that doesn't do full init.
  * @retval 1 if card is present and ready, 0 otherwise
  */
int SDMMC1_IsCardPresent(void)
{
  if (sd_initialized == 0U)
  {
    return 0;
  }
  
  /* Check if card is still responding */
  HAL_SD_CardStateTypeDef state = HAL_SD_GetCardState(&hsd1);
  if (state == HAL_SD_CARD_ERROR)
  {
    /* Card was removed or has error - reset state */
    SDMMC1_ResetState();
    return 0;
  }
  
  return 1;
}

/**
  * @brief  Periodic check for SD card insertion/removal.
  *         Call this from main loop to support hot-plug.
  *         Rate-limited to avoid excessive polling.
  */
void SDMMC1_PollCardPresence(void)
{
  /* Hot-plug SD card detection is currently disabled.
   * HAL_SD_Init() is a blocking call that can take 100s of ms,
   * which causes USB to stall and host disconnects.
   * 
   * For now, SD card must be present at boot.
   * TODO: Implement non-blocking SD detection or use a separate task.
   */
  (void)0;  /* Suppress unused warning */
}

/**
  * @brief  Quick SD card detection without full init.
  *         Tries to init SD just enough to detect presence,
  *         then sets the init state for later use.
  * @retval 1 if SD card is present, 0 otherwise
  */
int SDMMC1_QuickDetect(void)
{
  /* If already initialized, we know card is present */
  if (sd_initialized != 0U)
  {
    return 1;
  }
  
  /* Try a quick init - if it works, card is present */
  /* Note: This will fully init if successful */
  if (SDMMC1_SafeInit() == 0)
  {
    return 1;
  }
  
  return 0;
}

/* Internal init function with optional logging */
static int SDMMC1_DoInit(int quiet)
{
  /* Already initialized successfully */
  if (sd_initialized != 0U)
  {
    return 0;
  }

  if (!quiet)
  {
    LOG_INFO_TAG("SD", "Initializing SD card...");
  }

  /* IMPORTANT: SD cards MUST start in 1-bit mode, then switch to 4-bit */
  hsd1.Instance = SDMMC1;
  hsd1.Init.ClockEdge = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockPowerSave = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide = SDMMC_BUS_WIDE_1B;  /* Start with 1-bit */
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
  hsd1.Init.ClockDiv = 8;

  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    if (!quiet)
    {
      LOG_ERROR_TAG("SD", "HAL_SD_Init failed (no card?)");
    }
    /* Don't set sd_init_failed - allow retry for hot-plug */
    return -1;
  }

  /* Now switch to 4-bit mode for better performance */
  if (HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
  {
    LOG_WARN_TAG("SD", "4-bit mode failed, using 1-bit");
    /* Continue with 1-bit mode - still functional */
  }
  else
  {
    LOG_INFO_TAG("SD", "Switched to 4-bit mode");
  }

  /* Get card info for logging */
  HAL_SD_CardInfoTypeDef cardInfo;
  if (HAL_SD_GetCardInfo(&hsd1, &cardInfo) == HAL_OK)
  {
    uint32_t sizeMB = (uint32_t)((uint64_t)cardInfo.BlockNbr * cardInfo.BlockSize / 1048576ULL);
    LOG_INFO_TAG("SD", "Card: %lu blocks x %lu bytes = %lu MB",
                 (unsigned long)cardInfo.BlockNbr,
                 (unsigned long)cardInfo.BlockSize,
                 (unsigned long)sizeMB);
  }

  sd_initialized = 1U;
  LOG_INFO_TAG("SD", "SD card initialized successfully");
  return 0;
}

int SDMMC1_SafeInit(void)
{
  return SDMMC1_DoInit(0);  /* With logging */
}

int SDMMC1_SafeInitQuiet(void)
{
  return SDMMC1_DoInit(1);  /* Without logging */
}

int SDMMC1_IsInitialized(void)
{
  return (sd_initialized != 0U) ? 1 : 0;
}

/* USER CODE END 1 */
