/**
  ******************************************************************************
  * @file    led_status.h
  * @brief   LED status indicator header file
  ******************************************************************************
  */
#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdint.h>

/*
 * LED polarity:
 * Many WeAct/Core boards wire the LED to VDD, so the MCU pin must drive LOW to turn it ON.
 * Set to 0 if your board is active-high.
 */
#ifndef LED_STATUS_ACTIVE_LOW
#define LED_STATUS_ACTIVE_LOW 0U
#endif

/* Minimal LED API.
 * This project uses the blue LED primarily for fatal error signalling.
 */
void LED_Init(void);
void LED_On(void);
void LED_Off(void);

/* Fatal error signalling (blocks forever, repeats until reset).
 * Format:
 *  - 5s steady ON (attention)
 *  - stage pulses (count == stage)
 *  - gap
 *  - code pulses (count == code; code==0 encoded as 10 pulses)
 *  - long gap
 */
void LED_FatalStageCode(uint8_t stage, uint8_t code);

/* Convenience: stage = 0, only code is shown (code==0 encoded as 10 pulses). */
void LED_FatalCode(uint8_t code);

#endif /* LED_STATUS_H */
