# JPEG Encoder Library & Test Application

This directory contains a lightweight, portable C-based JPEG encoder designed for embedded systems and host applications. It specializes in converting **Raw Bayer** image data (e.g., from camera sensors) directly into JPEG format without requiring a full OS or heavy image processing framework.

## Overview

The library (`../jpeg_encoder.c`, `../jpeg_encoder.h`) provides a streamlined pipeline:
1.  **Input**: Raw binary streams or memory buffers (12-bit/16-bit Bayer data).
2.  **Processing**:
    *   **Offset Skipping**: Skips header/metadata lines automatically.
    *   **Black Level Subtraction**: Removes sensor black offset.
    *   **Demosaicing**: Simple bilinear interpolation (Bayer Low Complexity).
    *   **Color Conversion**: RGB to YCbCr (Internal).
    *   **Compression**: DCT-based JPEG encoding with configurable quality.
3.  **Output**: Standard JFIF JPEG stream.

## Building & Running the Test

The test application (`test.c`) demonstrates both file-stream and memory-buffer encoding.

```bash
# Build the test app
make clean
make

# Run the test
# Requires a raw input file named "frame_20260114.bin" in the directory
./test_app
```

---

## Library Usage

### 1. Integration
Include the header and add the source files to your build system:

*   `jpeg_encoder.h` (Public API)
*   `jpeg_encoder.c` (Implementation wrapper)
*   `JPEGENC.h` / `jpegenc.inl` (Core compression engine)

### 2. Basic Stream Encoding
Useful for encoding large files or data streams where you don't have enough RAM to hold the entire input/output.

```c
#include "jpeg_encoder.h"

// Define a simple stream interface
jpeg_stream_t stream;
stream.read = my_file_read_func;
stream.write = my_file_write_func;
stream.read_ctx = my_file_handle;   // Passed to read func
stream.write_ctx = my_output_file;  // Passed to write func

// Configure
jpeg_encoder_config_t config = {0};
config.width = 640;
config.height = 400;
config.pixel_format = JPEG_PIXEL_FORMAT_BAYER12_GRGB; // 12-bit Raw
config.bayer_pattern = JPEG_BAYER_PATTERN_GRGB;       // Sensor Pattern
config.quality = 90;
config.start_offset_lines = 2; // Skip 2 lines of metadata at start

// Encode
if (jpeg_encode_stream(&stream, &config) == 0) {
    printf("Success!");
}
```

### 3. Memory Buffer Encoding
Useful when the entire image is already in RAM (e.g., DMA transfer complete).

```c
uint8_t* raw_data = ...; // Your source
uint8_t* jpg_out = malloc(MAX_SIZE);
size_t jpg_size = 0;

int res = jpeg_encode_buffer(raw_data, raw_data_len, 
                            jpg_out, MAX_SIZE, &jpg_size, 
                            &config);
```

---

## Configuration Parameters

The `jpeg_encoder_config_t` structure controls the behavior:

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `width` | `uint16_t` | Image width in pixels. |
| `height` | `uint16_t` | Image height in pixels. |
| `pixel_format` | `enum` | **Crucial**. Defines how bytes are interpreted. <br> - `JPEG_PIXEL_FORMAT_BAYER12_GRGB`: Standard 16-bit container, 12-bit data. <br> - `JPEG_PIXEL_FORMAT_PACKED12`: MIPI packed (future support). |
| `bayer_pattern` | `enum` | Defines the starting color filter layout (RGGB, BGGR, etc.). **Must match your sensor HW configuration** or colors will look wrong/swapped. |
| `quality` | `int` | JPEG Quality (0-100). Higher = larger file, better looking. Typical embedded sweet spot: 75-90. |
| `start_offset_lines` | `int` | Number of **lines** (rows) to skip at the beginning of the binary stream. Useful if the sensor dumps status lines or metadata before the pixel data. |
| `ob_value` | `uint16_t` | Optical Black Value. Subtracted from every pixel to normalize black level (e.g., 64 or 240 depending on sensor). |
| `subtract_ob` | `bool` | Enable/Disable black level subtraction. |
| `enable_fast_mode` | `bool` | Enable optimized fixed-point math for color conversion/demosaicing. Faster, but might have slight precision differences compared to float reference. |

### Expected Binary Type (Input)
The current implementation primarily supports **Unpacked 16-bit Little Endian**.
*   **12-bit Bayer**: Each pixel occupies 2 bytes (uint16_t).
*   Values range from 0 to 4095.
*   The library automatically shifts these down to 8-bit for JPEG encoding (configurable internal shift).

---

## Porting Guide (Embedded & ARM)

This library is designed for portability. It avoids complex OS dependencies.

### 1. Requirements
*   **Standard Library**: `memcpy`, `memset`, `malloc`, `free`.
*   **Stack**: ~2-4KB stack usage (check `demosaic_row` and recursion in internal JPEG engine).
*   **Heap**: Dynamic allocation is used for row buffers (`strip`) and internal structures.
    *   640x400 Resolution ~ **42KB Heap** required.

### 2. Porting to STM32H5 / Cortex-M7 / M33
The code compiles with standard GCC for ARM.

**Steps:**
1.  **Memory Management**:
    *   If your MCU has limited heap, you can replace `malloc`/`free` calls in `jpeg_encoder.c` with static buffers or a custom pool allocator.
    *   Look for `jpeg_encoder_estimate_memory_requirement` to reserve the correct static size.
2.  **File I/O**:
    *   The `jpeg_stream_t` interface is abstract. For an MCU, map `read` to a DMA buffer read or Flash read, and `write` to a UART transmit, SD Card write, or simply a memory copy.
3.  **Endianness**:
    *   The code assumes Little Endian (Standard for ARM Cortex-M). If using Big Endian, `unpack_row` in `jpeg_encoder.c` needs modification to swap bytes.

### 3. Porting to ARM64 / AArch64 (Linux/Android)
No changes required. Compile with `-O2` or `-O3`. The standard libc handles everything.

---

## Performance Optimization

For high-performance scenarios (e.g., 30fps encoding):

1.  **Compiler Flags**:
    *   Always use `-O3` and link time optimization (`-flto`).
    *   For Cortex-M: `-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard`.

2.  **Fast Mode (Fixed Point Optimization)**:
    *   Enable `config.enable_fast_mode = true` to use the integer-based optimized path.
    *   **Demosaicing**: Uses Q8 fixed-point arithmetic (`x256`) for color gain application instead of floating point.
    *   **Bilinear Interpolation**: Uses integer division and shifting instead of float division.
    *   This provides a significant speedup on MCUs without hardware FPU or where integer pipelines are deeper.

3.  **Demosaicing (`demosaic_row`)**:
    *   This is the hottest loop. It iterates every pixel.
    *   **Optimization**: Rewrite `demosaic_row` using **SIMD** instructions (ARM Neon for AArch64, Arm Helium for M55/M85).
    *   Since vectors process 8 or 16 pixels at once, you can perform the neighbor averaging much faster than the scalar C code.

3.  **DCT / Quantization**:
    *   The core `jpegenc.inl` uses integer math.
    *   On MCUs with DSP extensions (Cortex-M4/M7/M33), ensure the compiler is generating `SMLAL` (Mac) instructions.

4.  **DMA**:
    *   On microcontrollers, implement the `stream->read` callback to read from a Peripheral (Camera Interface) DMA buffer directly, rather than copying data around.

## Memory Safety

The library includes a safety check:
```c
#define JPEG_ENCODER_MAX_MEMORY_USAGE (64 * 1024)
```
If the configuration (resolution + buffers) exceeds this limit during `jpeg_encode_stream`, it returns error `-10`. Adjust this macro in `jpeg_encoder.h` if you are working with higher resolutions (e.g., > VGA) or have more RAM available.

---

## Error Codes & Troubleshooting

The encoder returns **unique negative error codes**. You can fetch full details using:

```c
jpeg_encoder_error_t err;
if (jpeg_encoder_get_last_error(&err) == 0) {
    printf("code=%d, msg=%s, at %s:%d\n", err.code, err.message, err.function, err.line);
}
```

### Error Code Reference

| Code | Name | Meaning | How to Resolve |
| :--- | :--- | :--- | :--- |
| `-1` | `JPEG_ENCODER_ERR_INVALID_ARGUMENT` | Stream or config pointer is null. | Ensure `jpeg_encode_stream()` gets a valid stream with `read`/`write`, and a non-null config. |
| `-2` | `JPEG_ENCODER_ERR_INVALID_DIMENSIONS` | `width` or `height` is zero/invalid. | Confirm image dimensions are correct and set in `config`. |
| `-3` | `JPEG_ENCODER_ERR_INVALID_STRIDE` | Pixel format produced a zero/invalid stride. | Check `pixel_format` and make sure it matches the sensor output. |
| `-4` | `JPEG_ENCODER_ERR_MEMORY_LIMIT_EXCEEDED` | Estimated memory exceeds `JPEG_ENCODER_MAX_MEMORY_USAGE`. | Lower resolution, reduce buffers, or increase the macro limit in `jpeg_encoder.h`. |
| `-5` | `JPEG_ENCODER_ERR_OFFSET_EOF` | File ended while skipping offset lines. | Reduce `start_offset_lines` or check that input size includes the header + image data. |
| `-6` | `JPEG_ENCODER_ERR_JPEG_INIT_FAILED` | JPEG core failed to initialize. | Verify build includes `jpegenc.inl` and ensure `quality` and `pixel_format` are valid. |
| `-7` | `JPEG_ENCODER_ERR_ALLOC_RAW_BUFFER` | Failed to allocate raw input buffer. | Ensure heap size is sufficient or replace `malloc` with static buffers. |
| `-8` | `JPEG_ENCODER_ERR_ALLOC_UNPACK_BUFFER` | Failed to allocate unpacked 16-bit strip. | Same as above; reduce resolution or change memory strategy. |
| `-9` | `JPEG_ENCODER_ERR_ALLOC_RGB_BUFFER` | Failed to allocate RGB conversion buffer. | Same as above; also ensure `width` is not too large. |
| `-10` | `JPEG_ENCODER_ERR_ALLOC_CARRY_BUFFER` | Failed to allocate carry-over row buffer. | Same as above. |
| `-11` | `JPEG_ENCODER_ERR_ALLOC_LOOKAHEAD_BUFFER` | Failed to allocate lookahead row buffer. | Same as above. |
| `-12` | `JPEG_ENCODER_ERR_WRITE_OVERFLOW` | Output buffer too small (memory-buffer mode). | Increase `out_capacity` or lower `quality`. |
| `-13` | `JPEG_ENCODER_ERR_NULL_OUT_SIZE` | `out_size` pointer is null. | Pass a valid `size_t*` for output size. |
| `-14` | `JPEG_ENCODER_ERR_NULL_IN_BUFFER` | Input buffer pointer is null. | Ensure you provide a valid input pointer. |
| `-15` | `JPEG_ENCODER_ERR_NULL_OUT_BUFFER` | Output buffer pointer is null. | Provide a valid output buffer. |
| `-16` | `JPEG_ENCODER_ERR_ZERO_OUT_CAPACITY` | Output buffer size is zero. | Allocate a real buffer and pass its size. |

### Quick Debugging Checklist

1. **Check input file size**: must be `width * height * bytes_per_pixel + header_offset`. 
2. **Confirm `pixel_format`**: mismatch will produce wrong stride or incorrect colors. 
3. **Validate Bayer pattern**: wrong pattern leads to color artifacts but not necessarily a fatal error. 
4. **Memory usage**: use `jpeg_encoder_estimate_memory_requirement()` to check heap needs. 
5. **Buffer output size**: set `out_capacity` large enough (safe default = `width * height * 2`).
