# STM32H5 JPEG Encoder Optimization Plan

This document outlines the strategy to optimize the JPEG encoder for the STM32H5 microcontroller (Cortex-M33).

## 1. Hardware Capabilities of STM32H5
*   **Core**: ARM Cortex-M33 (ARMv8-M Mainline).
*   **DSP Extensions**: Available (if enabled by compiler/core). Supports SIMD instructions like `__SADD16`, `__SMLAD` (Dual Multiply-Accumulate).
*   **Helium (MVE)**: Not available.
*   **Hardware JPEG Codec**: Not available on STM32H5.
*   **ART Accelerator**: Accelerates instruction fetch from Flash (0-wait state execution). Does not provide computational offload.

## 2. Optimization Strategy

### A. Demosaicing (Major Gain)
The current implementation of `demosaic_row_bilinear` uses single-precision floating-point math (`float`) for gain application:
```c
float r_f = (float)r * r_gain;
```
**Optimization**: Replace with fixed-point arithmetic using integer units.
*   Use Q8 or Q12 fixed-point formulation.
*   Avoid FPU context switching and float-to-int conversion penalties.
*   Uses standard ALU instructions (single cycle).

### B. Color Conversion (RGB -> YCbCr)
Functions `JPEGSample16`/`JPEGSample24` perform matrix multiplication.
**Optimization**:
*   Ensure the compiler uses DSP instructions (`SMLAD` or `SMULWB`) if possible.
*   The current integer implementation is already efficient but can be verified against `arm_color_convert` functions if CMSIS-DSP were linked.
*   We retain the efficient integer implementation.

### C. DCT (Discrete Cosine Transform)
The function `JPEGFDCT` uses integer arithmetic with Q8 scaling (multipliers like 181 for 0.707).
**Optimization**:
*   Unroll loops explicitly to help the compiler pipeline instructions.
*   Use `int` (32-bit) for all accumulators to match the native register width of Cortex-M33, avoiding unnecessary `sxth` (sign extend halfword) instructions.
*   Use `__attribute__((always_inline))` for critical paths.

## 3. Implementation Plan
The code changes are guarded by `#ifdef FASTMODE`.

1.  **Define `FASTMODE`**: Ensure this is defined in your build system (CMake/Makefile) when targeting the chip.
2.  **`jpeg_encoder.c`**: 
    *   Rewrote `demosaic_row_bilinear` to use integer math.
3.  **Compiler Flags**:
    *   Ensure `-O3` or `-Ofast`.
    *   Ensure `-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16` (or similar) is set.
    *   Ensure CMSIS headers are in the include path if using intrinsics (though we use standard C that compiles to them).

## 4. Verification
*   Compile with `-DFASTMODE`.
*   Measure cycles for `jpeg_encoder_process` on target.
*   Verify image quality is identical (or negligible difference due to fixed-point rounding).
