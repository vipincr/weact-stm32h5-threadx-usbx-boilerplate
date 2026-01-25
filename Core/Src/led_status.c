/*
 * Minimal LED implementation.
 * Focus: reliable fatal error signalling (pulse counts, repeats forever).
 */

/* Includes ------------------------------------------------------------------*/
#include "led_status.h"
#include "main.h"
#include "stm32h5xx_hal.h"

/* Private function prototypes -----------------------------------------------*/
static void LED_Delay(uint32_t delay_ms);
static void LED_PulseCount(uint8_t count);
static void LED_Delay_DWT(uint32_t delay_ms);

/**
  * @brief  Turn the status LED on
  * @retval None
  */
void LED_On(void)
{
#if (LED_STATUS_ACTIVE_LOW != 0U)
  HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_SET);
#endif
}

/**
  * @brief  Turn the status LED off
  * @retval None
  */
void LED_Off(void)
{
#if (LED_STATUS_ACTIVE_LOW != 0U)
  HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_SET);
#else
  HAL_GPIO_WritePin(BLUE_LED_GPIO_Port, BLUE_LED_Pin, GPIO_PIN_RESET);
#endif
}
void LED_Init(void)
{
  /* GPIO must be configured by MX_GPIO_Init(). */
  LED_Off();
}

void LED_FatalCode(uint8_t code)
{
  LED_FatalStageCode(0U, code);
}

void LED_FatalStageCode(uint8_t stage, uint8_t code)
{
  /* Repeat forever so the user cannot miss the preamble/pulse counts. */
  for (;;)
  {
    LED_Off();
    LED_Delay(200U);

    /* Attention */
    LED_On();
    LED_Delay(5000U);
    LED_Off();
    LED_Delay(1000U);

    if (stage != 0U)
    {
      LED_PulseCount(stage);
      LED_Delay(1500U);
    }

    LED_PulseCount(code);

    /* Long gap between repeats */
    LED_Delay(3000U);
  }
}

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Simple delay using HAL_Delay
  * @param  delay_ms: Delay in milliseconds
  * @retval None
  */
static void LED_Delay(uint32_t delay_ms)
{
  /* Primary: use HAL timebase so delays match wall time.
   * During early ThreadX bring-up (or fault handlers) interrupts may be disabled,
   * and HAL_Delay would hang forever. In that case, fall back to DWT cycle counter
   * so we can still observe fatal error codes.
   */
  if (__get_PRIMASK() != 0U)
  {
    LED_Delay_DWT(delay_ms);
    return;
  }

  HAL_Delay(delay_ms);
}

static void LED_Delay_DWT(uint32_t delay_ms)
{
  if (delay_ms == 0U)
  {
    return;
  }

  /* Enable DWT CYCCNT if not already enabled. */
  if ((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U)
  {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  }
  if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
  {
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  }

  uint32_t hclk_hz = HAL_RCC_GetHCLKFreq();
  if (hclk_hz == 0U)
  {
    hclk_hz = SystemCoreClock;
  }
  if (hclk_hz == 0U)
  {
    /* Last resort: match tx_initialize_low_level.S default. */
    hclk_hz = 250000000U;
  }

  const uint32_t cycles_per_ms = (hclk_hz / 1000U);
  const uint32_t total_cycles = cycles_per_ms * delay_ms;
  const uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < total_cycles)
  {
    __NOP();
  }
}

static void LED_PulseCount(uint8_t count)
{
  /* Encode 0 as 10 pulses so "zero" is still visible. */
  uint8_t pulses = (count == 0U) ? 10U : count;

  for (uint8_t i = 0U; i < pulses; i++)
  {
    LED_On();
    LED_Delay(250U);
    LED_Off();
    LED_Delay(250U);
  }
}
