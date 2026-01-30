/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "app_threadx.h"
#include "main.h"
#include "cordic.h"
#include "dcache.h"
#include "fmac.h"
#include "icache.h"
#include "rtc.h"
#include "usb.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "logger.h"
#include "led_status.h"
#include "sdmmc.h"

#include "app_usbx_device.h"
#include "ux_device_cdc_acm.h"
#include "ux_api.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* Updated by main() at key checkpoints so Error_Handler can report where we died. */
volatile uint8_t g_boot_stage = 0U;

/* Reset cause flags captured at boot */
volatile uint32_t g_last_reset_flags = 0U;

/* Reboot counter stored in backup register 0 */
volatile uint32_t g_reboot_count = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

static void MX_CRS_Init_For_USB(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Extern declarations for RTC backup functions */
extern uint32_t RTC_ReadBackupRegister(uint32_t regIndex);
extern void RTC_WriteBackupRegister(uint32_t regIndex, uint32_t value);

#define BKUP_REBOOT_COUNTER_IDX  0U
#define BKUP_MAGIC_IDX           1U
#define BKUP_MAGIC_VALUE         0xDEADBEEFU

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  g_boot_stage = 1U;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* Early stabilization delay - allows power rails and clocks to settle */
  for (volatile uint32_t i = 0; i < 500000U; i++) { __NOP(); }
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  g_boot_stage = 2U;
  
  /* Post-clock stabilization delay (100ms) */
  HAL_Delay(100U);

  /* Check and log reset cause */
  {
    uint32_t reset_flags = RCC->RSR;
    if (reset_flags & RCC_RSR_IWDGRSTF) {
      /* IWDG reset - log later when CDC is ready */
    } else if (reset_flags & RCC_RSR_WWDGRSTF) {
      /* WWDG reset */
    } else if (reset_flags & RCC_RSR_SFTRSTF) {
      /* Software reset */
    } else if (reset_flags & RCC_RSR_BORRSTF) {
      /* Brown-out reset */
    } else if (reset_flags & RCC_RSR_PINRSTF) {
      /* Pin reset (NRST) */
    }
    /* Store for later logging */
    g_last_reset_flags = reset_flags;
    /* Clear reset flags */
    __HAL_RCC_CLEAR_RESET_FLAGS();
  }

  /* USB FS uses HSI48; enable CRS trimming (matches WeAct examples). */
  MX_CRS_Init_For_USB();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  /* MX_ICACHE_Init(); */
  MX_RTC_Init();
  MX_CORDIC_Init();
  /* MX_DCACHE1_Init(); */
  MX_FMAC_Init();
  /* USER CODE BEGIN 2 */
  g_boot_stage = 3U;

  LED_Init();

  /* Minimal boot sign-of-life (single short blink). */
  LED_On();
  HAL_Delay(100U);
  LED_Off();
  /* Leave LED OFF here; the USBX device thread will drive it so
   * a solid ON indicates USB bring-up, and OFF indicates USB IRQ traffic.
   */
  
  /* Track reboot count using RTC backup registers (persists across resets) */
  HAL_PWR_EnableBkUpAccess();
  if (RTC_ReadBackupRegister(BKUP_MAGIC_IDX) != BKUP_MAGIC_VALUE) {
    /* First boot after power cycle - initialize */
    RTC_WriteBackupRegister(BKUP_MAGIC_IDX, BKUP_MAGIC_VALUE);
    RTC_WriteBackupRegister(BKUP_REBOOT_COUNTER_IDX, 0U);
    g_reboot_count = 0U;
  } else {
    /* Subsequent reset - increment counter */
    g_reboot_count = RTC_ReadBackupRegister(BKUP_REBOOT_COUNTER_IDX) + 1U;
    RTC_WriteBackupRegister(BKUP_REBOOT_COUNTER_IDX, g_reboot_count);
  }
  
  /* Log boot messages - these will be buffered until CDC connects */
  LOG_INFO_TAG("BOOT", "System Reset #%lu", g_reboot_count);

  /* Always enable MSC for hot-plug support.
   * SD card presence is checked dynamically in MSC callbacks.
   * This allows SD card to be inserted/removed at runtime.
   */
  g_boot_stage = 4U;
  USBD_MSC_SetEnabled(1);  /* Always enable MSC class */
  
  /* Try to init SD card if present (non-blocking check) */
  if (SDMMC1_QuickDetect())
  {
    LOG_INFO_TAG("BOOT", "SD card detected at boot");
  }
  else
  {
    LOG_INFO_TAG("BOOT", "No SD card at boot (hot-plug supported)");
  }

  /* Next: ThreadX/USBX init */
  g_boot_stage = 5U;

#if defined(USBX_STANDALONE_BRINGUP)
  /* USBX standalone bring-up (no ThreadX). */
  /* Initialize Logger first (buffers etc) */
  Logger_Init();
  
  (void)MX_USBX_Device_Standalone_Init();
  MX_USB_PCD_Init();

  /* Configure PMA (Packet Memory Area) for endpoints.
   * STM32H5 USB has 2KB PMA. BDT (Buffer Descriptor Table) uses the first 
   * 8 entries × 8 bytes = 64 bytes (0x00-0x3F).
   * Endpoint buffers must start at 0x40 or later.
   *
   * Layout (64-byte buffers each):
   * 0x040: EP0 OUT (64 bytes)
   * 0x080: EP0 IN  (64 bytes)
   * 0x0C0: MSC OUT (EP1 OUT) (64 bytes)
   * 0x100: MSC IN  (EP1 IN)  (64 bytes)
   * 0x140: CDC DATA OUT (EP3 OUT) (64 bytes)
   * 0x180: CDC DATA IN  (EP3 IN)  (64 bytes)
   * 0x1C0: CDC CMD  IN  (EP2 IN)  (8 bytes, but allocate 64 for alignment)
   */
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x00, PCD_SNG_BUF, 0x40);
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x80, PCD_SNG_BUF, 0x80);
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x01, PCD_SNG_BUF, 0xC0);
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x81, PCD_SNG_BUF, 0x100);
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x03, PCD_SNG_BUF, 0x140);
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x83, PCD_SNG_BUF, 0x180);
  HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS, 0x82, PCD_SNG_BUF, 0x1C0);

  ux_dcd_stm32_initialize((ULONG)USB_DRD_FS, (ULONG)&hpcd_USB_DRD_FS);
  HAL_PCD_Start(&hpcd_USB_DRD_FS);
#endif
  /* USER CODE END 2 */

#if !defined(USBX_STANDALONE_BRINGUP)
  MX_ThreadX_Init();
#endif

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#if defined(USBX_STANDALONE_BRINGUP)
    /* Standalone USBX needs periodic polling. */
    ux_system_tasks_run();
    
    /* Poll for SD card insertion/removal (rate-limited internally to every 500ms) */
    SDMMC1_PollCardPresence();
    
    /* Poll CDC line state to detect DTR changes (reconnection). */
    USBD_CDC_ACM_PollLineState();
    
    Logger_Run();
    
    HAL_Delay(1U);
#endif
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Configure LSE Drive Capability
  *  Warning : Only applied when the LSE is disabled.
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_MEDIUMHIGH);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSE
                              |RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_CSI;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.CSIState = RCC_CSI_ON;
  RCC_OscInitStruct.CSICalibrationValue = RCC_CSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL1_SOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 125;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1_VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the programming delay
  */
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);
}

/* USER CODE BEGIN 4 */

static void MX_CRS_Init_For_USB(void)
{
  RCC_CRSInitTypeDef RCC_CRSInitStruct = {0};

  /* Enable the CRS APB clock */
  __HAL_RCC_CRS_CLK_ENABLE();

  /* Configure CRS to synchronize HSI48 to USB SOF (1kHz). */
  RCC_CRSInitStruct.Prescaler = RCC_CRS_SYNC_DIV1;
  RCC_CRSInitStruct.Source = RCC_CRS_SYNC_SOURCE_USB;
  RCC_CRSInitStruct.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  RCC_CRSInitStruct.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
  RCC_CRSInitStruct.ErrorLimitValue = 34;
  RCC_CRSInitStruct.HSI48CalibrationValue = 32;

  HAL_RCCEx_CRSConfig(&RCC_CRSInitStruct);
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* Report fatal HAL errors using the LED and stop. */
  LED_FatalStageCode((uint8_t)g_boot_stage, 1U);
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
