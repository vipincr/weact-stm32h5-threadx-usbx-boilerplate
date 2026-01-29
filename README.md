# STM32H5 USB Composite (ThreadX + USBX)

This repository targets the WeAct STM32H5 Core board (STM32H562RGT6) and is structured as an Azure RTOS (ThreadX) + USBX firmware project exposing a **USB composite device**:

- **USB CDC ACM**: virtual UART over USB for logging.
- **USB Mass Storage**: SD card (SDMMC1) read/write over USB.

Board reference: https://github.com/WeActStudio/WeActStudio.STM32H5_64Pin_CoreBoard

## Board summary

- MCU: STM32H562RGT6 (Cortex‑M33, 1MB Flash, 640KB SRAM)
- USB: USB 2.0 full‑speed device
- microSD slot: SDMMC1
- User LED and button
- 8 MHz HSE and 32.768 kHz LSE crystals
- USB Type‑C connector, SWD + UART header, dual 30‑pin GPIO headers

## Pin mapping (board + project)

The board documentation defines the default mapping. The current STM32CubeMX configuration matches it:

| Function | Pin(s) | Source |
|---|---|---|
| User LED | PB2 | [Core/Inc/main.h](Core/Inc/main.h) |
| User Button | PC13 | [Core/Inc/main.h](Core/Inc/main.h) |
| USB FS D- / D+ | PA11 / PA12 | Board docs |
| SDMMC1 | PC12 (CK), PD2 (CMD), PC8/PC9/PC10/PC11 (D0‑D3) | [Core/Src/sdmmc.c](Core/Src/sdmmc.c) |
| UART debug header | PA10 (RX), PA9 (TX) | Board docs |

## Current firmware configuration (from source)

### Clock tree

- System clock: PLL1 @ **240 MHz** sourced from **8 MHz HSE**.
- LSE and HSI48 enabled (USB uses HSI48).

Defined in [Core/Src/main.c](Core/Src/main.c).

### ThreadX

- Static allocation enabled (`USE_STATIC_ALLOCATION = 1`).
- Byte pools: 1KB for app + 1KB for USBX device.

Configured in [AZURE_RTOS/App/app_azure_rtos.c](AZURE_RTOS/App/app_azure_rtos.c) and [AZURE_RTOS/App/app_azure_rtos_config.h](AZURE_RTOS/App/app_azure_rtos_config.h).

### USB device stack (USBX)


- USBX system memory: 5KB (`USBX_DEVICE_MEMORY_STACK_SIZE`).
- USB device stack initialized with framework descriptors and string framework.
- USB composite device: **CDC ACM + MSC**.

Implementation in [USBX/App/app_usbx_device.c](USBX/App/app_usbx_device.c) and descriptors in [USBX/App/ux_device_descriptors.c](USBX/App/ux_device_descriptors.c).

### USB peripheral (PCD)

- Device‑only FS (USB_DRD_FS), 8 endpoints.
- HSI48 USB clock, no VBUS sensing.

Defined in [Core/Src/usb.c](Core/Src/usb.c).

### SDMMC (microSD)

- SDMMC1, 4‑bit bus, hardware flow control enabled.
- Clock divider = 8.

Defined in [Core/Src/sdmmc.c](Core/Src/sdmmc.c).

### FatFs filesystem

- FatFs R0.15 with exFAT support (`FF_FS_EXFAT = 1`).
- Multi-partition support for GPT-formatted SD cards (`FF_MULTI_PARTITION = 1`).
- 64-bit LBA for large drives (`FF_LBA64 = 1`).
- ThreadX reentrant configuration (`FF_FS_REENTRANT = 1`).
- Read-only mode (`FF_FS_READONLY = 1`) - firmware only monitors, doesn't write.

Configuration in [Core/Inc/ffconf.h](Core/Inc/ffconf.h), disk I/O in [Core/Src/sd_diskio.c](Core/Src/sd_diskio.c).

## USB composite device status

The device enumerates as a composite device:

- **CDC ACM**: virtual serial port used for logging output.
- **MSC**: exposes the SD card as a Mass Storage device (if SD card is present at boot).

See [USBX/App/ux_device_descriptors.c](USBX/App/ux_device_descriptors.c) and [USBX/App/app_usbx_device.c](USBX/App/app_usbx_device.c).

### Logging subsystem

The firmware includes a ring buffer-based logger that outputs colored messages to the CDC ACM interface:

- **Ring buffer**: 2KB buffer preserves boot logs until a terminal connects.
- **DTR detection**: Logs only flush when the terminal sets DTR (Data Terminal Ready).
- **Colored output**: ANSI escape codes for log levels:
  - ERROR: Red
  - WARN: Yellow
  - INFO: Green
  - DEBUG: Cyan
- **Boot log flush**: A one-shot thread waits 5 seconds then outputs "Logger initialized", flushing all buffered boot messages.

Usage:
```c
LOG_INFO("Simple message");
LOG_INFO_TAG("TAG", "Message with value: %d", value);
LOG_ERROR_TAG("MSC", "SD card read failed");
```

Implementation: [Core/Src/logger.c](Core/Src/logger.c) and [Core/Inc/logger.h](Core/Inc/logger.h).

### LED status behavior

The BLUE LED (PB2) is used as a status indicator:

- **Off**: CDC is not active.
- **Long blink**: USB is configured and CDC is present, but the host has not opened the CDC port yet (DTR not asserted).
- **Solid on**: host opened the CDC port (DTR asserted).

Implementation: [Core/Src/led_status.c](Core/Src/led_status.c) and CDC line-state handling in [USBX/App/ux_device_cdc_acm.c](USBX/App/ux_device_cdc_acm.c).

### SD card / MSC behavior

- MSC class is only registered if an SD card is detected at boot.
- If no SD card is present, the device enumerates as CDC-only.
- Hot-plug of SD cards is not currently supported.

### Filesystem monitoring

The firmware includes a FatFs-based filesystem reader with change detection:

- **FatFs R0.15**: Industry-standard filesystem library with exFAT and GPT partition support.
- **Polling-based monitoring**: Scans the SD card every 5 seconds for changes.
- **Recursive scanning**: Monitors up to 4 directory levels deep, tracking up to 128 entries.
- **Callback notifications**: Register a callback to receive change events.

**Event types:**
- `FS_EVENT_FILE_CREATED` - New file detected
- `FS_EVENT_FILE_MODIFIED` - File size or timestamp changed
- `FS_EVENT_FILE_DELETED` - File removed
- `FS_EVENT_DIR_CREATED` - New directory detected
- `FS_EVENT_DIR_DELETED` - Directory removed

**Usage:**
```c
#include "fs_reader.h"

void my_change_handler(FS_EventType_t event, const char *path)
{
    LOG_INFO_TAG("APP", "Change: %s on %s", FS_Reader_EventTypeStr(event), path);
}

// Register custom callback (default logs to CDC)
FS_Reader_SetChangeCallback(my_change_handler);
```

**MSC conflict handling**: When the host computer accesses the SD card via USB MSC, disk read errors may occur. The filesystem monitor detects these errors and skips that polling cycle to avoid false change reports.

Implementation: [Core/Src/fs_reader.c](Core/Src/fs_reader.c) and [Core/Inc/fs_reader.h](Core/Inc/fs_reader.h).

### Timing utility

The firmware includes timing macros for measuring function execution time:

```c
#include "time_it.h"

// Measure execution time in milliseconds (1ms resolution)
uint32_t elapsed_ms;
TIME_IT(elapsed_ms, my_function(arg1, arg2));
LOG_INFO_TAG("PERF", "Took %lu ms", (unsigned long)elapsed_ms);

// Measure and capture return value
uint32_t elapsed_ms;
int result;
TIME_IT_RET(elapsed_ms, result, my_function(arg1, arg2));

// Microsecond precision using DWT cycle counter
uint32_t elapsed_us;
TIME_IT_US(elapsed_us, fast_operation());
```

**Available macros:**
- `TIME_IT(elapsed_ms, func_call)` - Millisecond timing (HAL_GetTick)
- `TIME_IT_RET(elapsed_ms, ret_val, func_call)` - Millisecond timing with return value
- `TIME_IT_US(elapsed_us, func_call)` - Microsecond timing (DWT cycle counter)
- `TIME_IT_US_RET(elapsed_us, ret_val, func_call)` - Microsecond timing with return value

Implementation: [Core/Inc/time_it.h](Core/Inc/time_it.h).

## How the system boots

1. HAL init + system clock setup.
2. GPIO, caches, RTC, CORDIC, FMAC are initialized.
3. SD card detection and initialization (if present).
4. ThreadX kernel starts (`MX_ThreadX_Init`).
5. In `tx_application_define`, ThreadX and USBX byte pools are created and USBX device stack is initialized.
6. Logger flush thread waits 5 seconds then triggers boot log flush.

See [Core/Src/main.c](Core/Src/main.c), [Core/Src/app_threadx.c](Core/Src/app_threadx.c), and [AZURE_RTOS/App/app_azure_rtos.c](AZURE_RTOS/App/app_azure_rtos.c).

## Project structure

- Azure RTOS app: [AZURE_RTOS/App](AZURE_RTOS/App)
- Core HAL and BSP init: [Core/Inc](Core/Inc) and [Core/Src](Core/Src)
- USBX device app: [USBX/App](USBX/App)
- USBX STM32 target config: [USBX/Target](USBX/Target)
- FatFs middleware: [Middlewares/Third_Party/FatFs](Middlewares/Third_Party/FatFs)
- FatFs configuration: [Core/Inc/ffconf.h](Core/Inc/ffconf.h)
- STM32CubeMX configuration: [WeActSTM32H5.ioc](WeActSTM32H5.ioc)

## Build (CMake)

### Prerequisites

- CMake 3.22+
- Ninja
- GNU Arm Embedded Toolchain (`arm-none-eabi-gcc` in PATH)

### Configure and build

- Debug:
   - `cmake --preset Debug`
   - `cmake --build --preset Debug`
- Release:
   - `cmake --preset Release`
   - `cmake --build --preset Release`

The main output is an ELF file:

- `build/Debug/WeActSTM32H5.elf`
- `build/Release/WeActSTM32H5.elf`

If you use the builder script (below), it will also generate `.bin` and `.hex` files using `arm-none-eabi-objcopy`.

## Builder script

Use `builder.sh` for clean/build/flash/monitor convenience. It uses the CMake presets in [CMakePresets.json](CMakePresets.json).

- Clean:
   - `./builder.sh clean`
- Build (Debug/Release):
   - `./builder.sh build --type Debug`
   - `./builder.sh build --type Release`
- Flash (auto-detect DFU, fallback to STM32CubeProgrammer):
   - `./builder.sh flash --address 0x08000000`
- All (clean → build → flash):
   - `./builder.sh all --type Debug --address 0x08000000`
- CDC monitor:
   - `./builder.sh monitor --device /dev/cu.usbmodemXXXX`

## Notes for contributors

- Generated code uses **USER CODE** regions. Keep changes inside those blocks to avoid being overwritten by STM32CubeMX regeneration.
- Update [WeActSTM32H5.ioc](WeActSTM32H5.ioc) if you change clocks, USB, SDMMC, or middleware configuration.
- The logger is self-contained - just use `LOG_*` macros, no special initialization needed by callers.

## Troubleshooting (macOS)

If `./builder.sh enumerate` shows no new USB device:

- Ensure you are using a **data-capable** USB-C cable (many charge-only cables exist).
- If you flashed via DFU, **reset the board** after flashing (DFU reset is not supported by this script).
- Run `system_profiler SPUSBDataType` (without filters) and search for any new device when you plug/unplug the board.
- Watch for new serial ports: `ls /dev/cu.* /dev/tty.* | grep -i "usb\|modem"`.

To invert LED polarity if needed (some boards differ), adjust `LED_STATUS_ACTIVE_LOW` in [Core/Inc/led_status.h](Core/Inc/led_status.h).
