# Copilot instructions

## Project context

- Target board: WeAct STM32H5 Core (STM32H562RGT6).
- RTOS: ThreadX (Azure RTOS).
- USB stack: USBX device (STM32 DCD).
- USB composite device: CDC ACM (virtual UART for logging) + MSC (SD card).

## Guardrails for generated code

- **Do not edit generated code outside USER CODE blocks.** Only place changes inside `/* USER CODE BEGIN */` … `/* USER CODE END */` regions unless explicitly requested.
- Keep STM32CubeMX configuration aligned in [WeActSTM32H5.ioc](WeActSTM32H5.ioc) when modifying peripherals, clocks, or middleware.
- Prefer small, targeted changes that do not alter public APIs.

## Where to look for core behavior

- ThreadX startup and memory pools: [AZURE_RTOS/App/app_azure_rtos.c](../AZURE_RTOS/App/app_azure_rtos.c)
- ThreadX init wrapper: [Core/Src/app_threadx.c](../Core/Src/app_threadx.c)
- USBX device init and class registration: [USBX/App/app_usbx_device.c](../USBX/App/app_usbx_device.c)
- USB descriptors: [USBX/App/ux_device_descriptors.c](../USBX/App/ux_device_descriptors.c)
- CDC ACM callbacks: [USBX/App/ux_device_cdc_acm.c](../USBX/App/ux_device_cdc_acm.c)
- MSC callbacks (SD card backing store): [USBX/App/ux_device_msc.c](../USBX/App/ux_device_msc.c)
- USB PCD init: [Core/Src/usb.c](../Core/Src/usb.c)
- SDMMC init: [Core/Src/sdmmc.c](../Core/Src/sdmmc.c)
- Logger: [Core/Src/logger.c](../Core/Src/logger.c) and [Core/Inc/logger.h](../Core/Inc/logger.h)
- LED status: [Core/Src/led_status.c](../Core/Src/led_status.c)

## Logging subsystem

The project uses a simple ring buffer logger that outputs to CDC ACM:

- **Ring buffer**: 2KB buffer stores log messages during boot before terminal connects.
- **DTR detection**: Logs only flush to CDC when the terminal is ready (DTR asserted).
- **Colored output**: ANSI escape codes for ERROR (red), WARN (yellow), INFO (green), DEBUG (cyan).
- **Boot log flush**: A one-shot thread waits 5 seconds after boot then triggers a log to flush buffered messages.

### Logger usage

```c
#include "logger.h"

LOG_INFO("Simple message");
LOG_INFO_TAG("TAG", "Message with args: %d", value);
LOG_ERROR_TAG("ERR", "Error occurred");
LOG_WARN_TAG("WARN", "Warning message");
LOG_DEBUG_TAG("DBG", "Debug info");
```

### Logger architecture

- `Logger_Log()` formats the message with color codes, writes to ring buffer, and attempts to flush.
- `ring_flush()` only outputs when `terminal_ready()` returns true (CDC connected with DTR set).
- This ensures boot logs are preserved until a terminal application connects.

## Coding conventions

- C language, STM32 HAL style.
- Respect HAL initialization patterns (`MX_*_Init`), and avoid adding blocking work inside ISRs.
- Keep configuration values in headers (`app_azure_rtos_config.h`, `ux_user.h`) rather than hard‑coding.
- **Encapsulation**: Subsystems (logger, LED, etc.) should be self-contained. Callers should not need internal knowledge.
- **Simplicity**: Prefer simple solutions without unnecessary semaphores, mutexes, or complex threading unless required.

## Build system

- CMake project using presets in [CMakePresets.json](../CMakePresets.json).
- Toolchain: [cmake/gcc-arm-none-eabi.cmake](../cmake/gcc-arm-none-eabi.cmake).
- Builder script: `./builder.sh build|flash|monitor|clean`

## Typical task expectations

- For USB composite updates, ensure both descriptor and class registration changes are made.
- For MSC, implement `USBD_STORAGE_*` callbacks to access the SD card and perform cache maintenance when DMA is used.
- Ensure required peripherals are initialized **before** USBX device stack starts.
- For logging, use `LOG_*_TAG()` macros - no special handling needed, the logger buffers until terminal connects.

## Verification

- **ALWAYS build after every code change.** Check for compilation errors using `./builder.sh build` and fix them before responding.
