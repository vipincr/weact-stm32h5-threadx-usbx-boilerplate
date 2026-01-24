# STM32H5 USB Composite (ThreadX + USBX)

This repository targets the WeAct STM32H5 Core board (STM32H562RGT6) and is structured as an Azure RTOS (ThreadX) + USBX firmware project intended to expose a **USB composite device**:

- **USB CDC ACM**: virtual UART over USB.
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
- **Only the MSC class is currently registered**.

Implementation in [USBX/App/app_usbx_device.c](USBX/App/app_usbx_device.c) and descriptors in [USBX/App/ux_device_descriptors.c](USBX/App/ux_device_descriptors.c).

### USB peripheral (PCD)

- Device‑only FS (USB_DRD_FS), 8 endpoints.
- HSI48 USB clock, no VBUS sensing.

Defined in [Core/Src/usb.c](Core/Src/usb.c).

### SDMMC (microSD)

- SDMMC1, 4‑bit bus, hardware flow control enabled.
- Clock divider = 8.

Defined in [Core/Src/sdmmc.c](Core/Src/sdmmc.c).

## USB composite device status

The **intended composite device** is CDC ACM + MSC. The current codebase **only enables MSC**:

- USB framework builder contains MSC descriptors only.
- `UserClassInstance[]` includes only `CLASS_TYPE_MSC`.
- MSC class is registered at runtime; CDC ACM is not registered.

See [USBX/App/ux_device_descriptors.c](USBX/App/ux_device_descriptors.c) and [USBX/App/app_usbx_device.c](USBX/App/app_usbx_device.c).

### What is implemented

- USBX device stack initialization.
- MSC class registration.
- Descriptor strings:
  - Manufacturer: “Gramini Labs”
  - Product: “Gramini JPEG Encoder”

From [USBX/App/ux_device_descriptors.h](USBX/App/ux_device_descriptors.h).

### What is still stubbed / missing

- **MSC backing store** is not implemented: all storage callbacks are stubs and return success with no SD access.
  - See [USBX/App/ux_device_msc.c](USBX/App/ux_device_msc.c).
- **CDC ACM class** is not registered and not present in descriptors.
- **Peripheral init calls** are missing in the main boot path:
  - `MX_USB_PCD_Init()` and `MX_SDMMC1_SD_Init()` are never called.

These gaps must be addressed before the composite device is functional.

## How the system boots

1. HAL init + system clock setup.
2. GPIO, caches, RTC, CORDIC, FMAC are initialized.
3. ThreadX kernel starts (`MX_ThreadX_Init`).
4. In `tx_application_define`, ThreadX and USBX byte pools are created and USBX device stack is initialized.

See [Core/Src/main.c](Core/Src/main.c), [Core/Src/app_threadx.c](Core/Src/app_threadx.c), and [AZURE_RTOS/App/app_azure_rtos.c](AZURE_RTOS/App/app_azure_rtos.c).

## Project structure

- Azure RTOS app: [AZURE_RTOS/App](AZURE_RTOS/App)
- Core HAL and BSP init: [Core/Inc](Core/Inc) and [Core/Src](Core/Src)
- USBX device app: [USBX/App](USBX/App)
- USBX STM32 target config: [USBX/Target](USBX/Target)
- STM32CubeMX configuration: [WeActSTM32H5.ioc](WeActSTM32H5.ioc)
- Keil MDK project: [MDK-ARM/WeActSTM32H5.uvprojx](MDK-ARM/WeActSTM32H5.uvprojx)

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
- UART monitor (debug header):
   - `./builder.sh monitor --device /dev/cu.usbserial-XXXX --baud 115200`

## Planned work to reach the composite device goal

1. **Enable CDC ACM in descriptors**
   - Add `CLASS_TYPE_CDC_ACM` to `UserClassInstance` and define CDC interfaces/endpoints in the framework builder in [USBX/App/ux_device_descriptors.c](USBX/App/ux_device_descriptors.c).
2. **Register CDC class at runtime**
   - Register CDC ACM via `ux_device_stack_class_register` in [USBX/App/app_usbx_device.c](USBX/App/app_usbx_device.c).
3. **Wire MSC callbacks to SDMMC1**
   - Implement `USBD_STORAGE_*` to call `HAL_SD_ReadBlocks/WriteBlocks` with cache maintenance as needed in [USBX/App/ux_device_msc.c](USBX/App/ux_device_msc.c).
4. **Initialize USB and SDMMC peripherals**
   - Ensure `MX_USB_PCD_Init()` and `MX_SDMMC1_SD_Init()` are called before USBX starts.

## Notes for contributors

- Generated code uses **USER CODE** regions. Keep changes inside those blocks to avoid being overwritten by STM32CubeMX regeneration.
- Update [WeActSTM32H5.ioc](WeActSTM32H5.ioc) if you change clocks, USB, SDMMC, or middleware configuration.
