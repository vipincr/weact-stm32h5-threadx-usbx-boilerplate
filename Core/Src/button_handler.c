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
#include "stm32h5xx_hal.h"

/* Private defines -----------------------------------------------------------*/
#define BUTTON_THREAD_STACK_SIZE  1024U
#define BUTTON_THREAD_PRIORITY    20U
#define BUTTON_POLL_MS            10U
#define BUTTON_DEBOUNCE_COUNT     5U    /* ~50ms at 10ms poll rate for noise rejection */

/* Private variables ---------------------------------------------------------*/
static TX_THREAD button_thread;
static UCHAR button_thread_stack[BUTTON_THREAD_STACK_SIZE];

/* Private function prototypes -----------------------------------------------*/
static VOID button_thread_entry(ULONG thread_input);

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
  * @brief  Button handler thread - polls USER_BUTTON and logs presses.
  * @param  thread_input: Thread input parameter (unused).
  *
  * Debounce algorithm:
  * - Track the current "stable" state (what we believe the button is)
  * - Count consecutive readings that differ from stable state
  * - Only transition when we get N consecutive different readings
  * - Only log on transition from released to pressed
  */
static VOID button_thread_entry(ULONG thread_input)
{
  TX_PARAMETER_NOT_USED(thread_input);

  GPIO_PinState stable_state = GPIO_PIN_RESET;  /* Assume released initially (active-HIGH button) */
  GPIO_PinState current_state;
  uint32_t consecutive_count = 0U;

  /* Wait for GPIO to stabilize after boot */
  tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / 2U);

  /* Read initial state */
  stable_state = HAL_GPIO_ReadPin(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);

  for (;;)
  {
    current_state = HAL_GPIO_ReadPin(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);

    if (current_state == stable_state)
    {
      /* Same as stable state - reset counter */
      consecutive_count = 0U;
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
          /* Transition from released to pressed (active-HIGH: pressed = SET) */
          LOG_INFO("Button pressed");
        }
        stable_state = current_state;
        consecutive_count = 0U;
      }
    }

    /* Poll every 10ms */
    tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / (1000U / BUTTON_POLL_MS));
  }
}
