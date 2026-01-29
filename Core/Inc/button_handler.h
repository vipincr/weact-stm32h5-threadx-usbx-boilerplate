/**
  ******************************************************************************
  * @file    button_handler.h
  * @brief   Button handler thread for USER_BUTTON
  ******************************************************************************
  */
#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include "tx_api.h"

/**
  * @brief  Initialize and start the button handler thread.
  * @param  byte_pool: Pointer to ThreadX byte pool for stack allocation,
  *                    or NULL to use static allocation.
  * @retval TX_SUCCESS on success, error code otherwise.
  */
UINT ButtonHandler_Init(TX_BYTE_POOL *byte_pool);

#endif /* BUTTON_HANDLER_H */
