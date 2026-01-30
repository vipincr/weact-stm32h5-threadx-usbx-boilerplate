# Copilot instructions

## Project context

- Target board: WeAct STM32H5 Core (STM32H562RGT6).
- RTOS: ThreadX (Azure RTOS).
- USB stack: USBX device (STM32 DCD).
- USB composite device: CDC ACM (virtual UART for logging) + MSC (SD card).
- JPEG processing: Converts Bayer RAW `.bin` files to JPEG on-device.
- Exclusive SD access: Button-controlled switching between FatFS (local) and MSC (host) modes.

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
- Filesystem reader/monitor: [Core/Src/fs_reader.c](../Core/Src/fs_reader.c) and [Core/Inc/fs_reader.h](../Core/Inc/fs_reader.h)
- FatFs configuration: [Core/Inc/ffconf.h](../Core/Inc/ffconf.h)
- FatFs disk I/O: [Core/Src/sd_diskio.c](../Core/Src/sd_diskio.c)
- Timing utility: [Core/Inc/time_it.h](../Core/Inc/time_it.h)
- Button handler (FSM mode switching): [Core/Src/button_handler.c](../Core/Src/button_handler.c)
- JPEG processor: [Core/Src/jpeg_processor.c](../Core/Src/jpeg_processor.c)
- SD adapter (exclusive access): [Core/Src/sd_adapter.c](../Core/Src/sd_adapter.c)

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

## Filesystem monitoring

The project includes FatFs-based filesystem monitoring with callback notifications:

- **FatFs R0.15**: Proven filesystem library with exFAT and GPT partition support.
- **Polling-based change detection**: Compares filesystem snapshots every 5 seconds.
- **Recursive monitoring**: Scans up to 4 directory levels, tracking up to 128 entries.
- **Callback architecture**: Users register a callback to receive change notifications.
- **MSC conflict handling**: Skips change detection when disk errors occur (USB MSC accessing the card).

### Filesystem monitoring usage

```c
#include "fs_reader.h"

// Custom callback (optional - default logs to CDC)
void my_handler(FS_EventType_t event, const char *path)
{
    // event: FS_EVENT_FILE_CREATED, FILE_MODIFIED, FILE_DELETED, DIR_CREATED, DIR_DELETED
    // path: full path like "/Hello/file.txt"
}

FS_Reader_SetChangeCallback(my_handler);
```

### Filesystem architecture

- `FS_Reader_Init()` creates a ThreadX thread (priority 25) that mounts FatFs and monitors.
- Two static snapshots (~17KB each) store filesystem state for comparison.
- On each poll cycle, a new snapshot is taken recursively and compared to the previous.
- Changes are reported via the registered callback (default: log to CDC).
- Disk errors during snapshot set `has_error` flag, causing that cycle to be skipped.

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

## Exclusive SD access (FatFS ↔ MSC)

FatFS and USB MSC cannot safely access the SD card simultaneously. The firmware uses exclusive mode switching:

- **FatFS mode** (default): Firmware owns SD for JPEG encoding. MSC reports "no media".
- **MSC mode**: Host owns SD via USB. FatFS unmounted.

### Mode switching via button

- **Double-click**: Toggle modes.
- **Single-click**: Process `.bin` files (FatFS mode only).

### Transition rules

| From | To | Pre-condition | Action |
|------|-----|---------------|--------|
| FatFS | MSC | None | Unmount FatFS, enable MSC |
| MSC | FatFS | Host ejected | Set FatFS mode, mount |
| MSC | FatFS | Host NOT ejected | Stay MSC, log warning |

### Key functions

- `SD_GetMode()` / `SD_SetMode()` - Query/set current mode
- `SD_IsEjected()` / `SD_SetEjected()` - Host eject detection
- `SD_IsMscAllowed()` - Returns true only in MSC mode and not ejected
- `USBD_STORAGE_EjectNotify()` - Called by modified USBX on SCSI eject command

## JPEG processor

Streaming JPEG encoder for Bayer RAW to JPEG conversion:

```c
#include "jpeg_processor.h"

JPEG_Processor_Status_t status = JPEG_Processor_ConvertFile("/input.bin", NULL);
// Output: /input.jpg
```

- Streaming architecture minimizes RAM usage
- Configurable quality, white balance gains
- Auto-generates output filename from input

## Typical task expectations

- For USB composite updates, ensure both descriptor and class registration changes are made.
- For MSC, implement `USBD_STORAGE_*` callbacks to access the SD card and perform cache maintenance when DMA is used.
- Ensure required peripherals are initialized **before** USBX device stack starts.
- For logging, use `LOG_*_TAG()` macros - no special handling needed, the logger buffers until terminal connects.
- For filesystem monitoring, use `FS_Reader_SetChangeCallback()` to register custom handlers.
- Large structures (>1KB) should be static, not stack-allocated, to avoid ThreadX stack overflow.
- FatFs and USB MSC use **exclusive access** - check mode with `SD_GetMode()` before operations.
- Button handler uses FSM - transitions are atomic and revert on error.
- **Never log from USBX callback context** - causes deadlock. Set flags instead.

## Timing utility

The project includes timing macros in [Core/Inc/time_it.h](../Core/Inc/time_it.h) for measuring function execution time:

```c
#include "time_it.h"

// Measure execution time in milliseconds
uint32_t elapsed_ms;
TIME_IT(elapsed_ms, my_function(arg1, arg2));
LOG_INFO_TAG("PERF", "Took %lu ms", (unsigned long)elapsed_ms);

// Measure with return value capture
uint32_t elapsed_ms;
int result;
TIME_IT_RET(elapsed_ms, result, my_function(arg1, arg2));

// Microsecond precision (DWT cycle counter)
uint32_t elapsed_us;
TIME_IT_US(elapsed_us, fast_operation());
```

- `TIME_IT` / `TIME_IT_RET`: 1ms resolution using `HAL_GetTick()`
- `TIME_IT_US` / `TIME_IT_US_RET`: Microsecond resolution using DWT cycle counter

## Verification

- **ALWAYS build after every code change.** Check for compilation errors using `./builder.sh build` and fix them before responding.
