#include "jpeg_encoder.h"
#include "jpeg_encoder_timing.h"
#include "JPEGENC.h"
#include "jpegenc.inl"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#if JPEG_TIMING_ENABLED
/* Global timing data instance */
jpeg_timing_t g_jpeg_timing;
#endif

/*
 * FASTMODE: STM32H5 / Cortex-M33 optimizations
 * - ARM ACLE intrinsics: __usat, __smlad, __ssub16, etc.
 * - Fixed-point arithmetic instead of float
 * - 4:2:2 subsampling for speed
 * - USAT for single-cycle saturation
 */
#if defined(FASTMODE)

#include <arm_acle.h>

/* GCC ARM uses lowercase intrinsics; map uppercase ACLE-style names */
#ifndef __SSUB16
#define __SSUB16 __ssub16
#endif
#ifndef __USUB16
#define __USUB16 __usub16
#endif
#ifndef __SMLAD
#define __SMLAD __smlad
#endif
#ifndef __UHADD16
#define __UHADD16 __uhadd16
#endif

#define JPEG_ENC_HAS_DSP  1
#define JPEG_ENC_USE_DSP  1

/* Single-cycle branchless saturation to 0-255 */
#define CLAMP_SAT(x) do { (x) = __usat((x), 8); } while(0)
static inline uint8_t clamp_u8(int v) { return (uint8_t)__usat(v, 8); }

#else  /* !FASTMODE */

#define JPEG_ENC_HAS_DSP  0
#define JPEG_ENC_USE_DSP  0

#define CLAMP_SAT(x) do { if ((x) < 0) (x) = 0; else if ((x) > 255) (x) = 255; } while(0)
static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

#endif /* FASTMODE */

#define JPEG_ENC_PACK16(a, b) ((int)(((uint16_t)(int16_t)(a)) | ((uint32_t)(uint16_t)(int16_t)(b) << 16)))

static inline int jpeg_enc_smlad_fallback(int a, int b, int acc)
{
    int a0 = (int16_t)a;
    int a1 = (int16_t)(a >> 16);
    int b0 = (int16_t)b;
    int b1 = (int16_t)(b >> 16);
    return acc + (a0 * b0) + (a1 * b1);
}

static float s_g_gain = 1.0f;
static int s_g_gain_fix = 256;
static uint8_t s_y_lut[256];
static int s_y_lut_ready = 0;

typedef struct {
    uint8_t* raw_file_chunk;
    size_t raw_size;
    uint16_t* unpacked_strip;
    size_t unpack_size;
    uint8_t* out_strip;
    size_t out_size;
    uint16_t* carry_over_row;
    size_t carry_size;
    uint16_t* lookahead_row_save;
    size_t lookahead_size;
} jpeg_encoder_workspace_t;

static jpeg_encoder_workspace_t s_workspace = {0};

static int jpeg_alloc_reuse(void** ptr, size_t* current_size, size_t needed_size)
{
    if (needed_size == 0) {
        return 1;
    }
    if (*ptr && *current_size >= needed_size) {
        return 1;
    }
    void* next = realloc(*ptr, needed_size);
    if (!next) {
        return 0;
    }
    *ptr = next;
    *current_size = needed_size;
    return 1;
}


// Bayer pattern lookup: [pattern][row_phase][x&1] -> 0=R,1=G,2=B
static const uint8_t s_bayer_color_lut[4][2][2] = {
    { {0, 1}, {1, 2} }, // RGGB
    { {2, 1}, {1, 0} }, // BGGR
    { {1, 0}, {2, 1} }, // GRBG
    { {1, 2}, {0, 1} }  // GBRG
};

// For green pixels: which rows contain red? [pattern][row_phase]
static const uint8_t s_row_has_red_lut[4][2] = {
    {1, 0}, // RGGB
    {0, 1}, // BGGR
    {1, 0}, // GRBG
    {0, 1}  // GBRG
};

static void init_y_lut(void)
{
    if (s_y_lut_ready) {
        return;
    }

    // Fast tone/contrast curve: mild gamma + contrast, baked into LUT
    // Keep it lightweight for real-time use.
    const float gamma = 0.92f;
    const float contrast = 1.10f;
    const float pivot = 128.0f;

    for (int i = 0; i < 256; ++i) {
        float x = (float)i / 255.0f;
        // gamma
        float g = powf(x, gamma);
        // contrast around pivot
        float y = (g * 255.0f - pivot) * contrast + pivot;
        if (y < 0.0f) y = 0.0f;
        if (y > 255.0f) y = 255.0f;
        s_y_lut[i] = (uint8_t)(y + 0.5f);
    }

    s_y_lut_ready = 1;
}

#if JPEG_ENC_HAS_DSP
#define JPEG_ENC_SMLAD(a, b, acc) __SMLAD((a), (b), (acc))
#else
#define JPEG_ENC_SMLAD(a, b, acc) jpeg_enc_smlad_fallback((a), (b), (acc))
#endif

#define JPEG_ENC_COEF_Y_RG  JPEG_ENC_PACK16(1225, 2404)
#define JPEG_ENC_COEF_CB_RG JPEG_ENC_PACK16(-691, -1357)
#define JPEG_ENC_COEF_CR_RG JPEG_ENC_PACK16(2048, -1715)
#define JPEG_ENC_COEF_Y_B   (467)
#define JPEG_ENC_COEF_CR_B  (-333)

/*
 * JPEG Encoding Adapter (C Implementation)
 * 
 * Pipeline:
 * 1. Unpack Raw Data -> 16-bit Intermediate (Native Sensor Range, e.g. 0-4095 or 0-65520)
 * 2. Subtract Black Level
 * 3. Demosaic (Bilinear Interpolation) -> 16-bit Intermediate RGB
 * 4. Apply White Balance Gains -> Float/Int
 * 5. Normalize/Tone Map -> 8-bit RGB (Scale/Shift)
 * 6. Encode JPEG
 */

// Helper to determine normalization shift based on pixel format
static jpeg_encoder_error_t g_last_error;

static void jpeg_set_error(jpeg_encoder_error_code_t code, const char* message, const char* function, int line) {
    g_last_error.code = code;
    g_last_error.message = message;
    g_last_error.function = function;
    g_last_error.line = line;
}

int jpeg_encoder_get_last_error(jpeg_encoder_error_t* out_error) {
    if (!out_error) return -(int)JPEG_ENCODER_ERR_INVALID_ARGUMENT;
    *out_error = g_last_error;
    return 0;
}

static int get_downshift_for_format(jpeg_pixel_format_t format) {
    switch (format) {
        case JPEG_PIXEL_FORMAT_PACKED12:
        case JPEG_PIXEL_FORMAT_UNPACKED12:
            return 4; // 12-bit (4095) -> 8-bit (255) : >> 4
        case JPEG_PIXEL_FORMAT_BAYER12_GRGB:
             // Note: If data is 0-4095, shift 4. If data is 0-65520 (MSB), shift 8.
             // Based on frame_20260114.bin analysis, it is MSB aligned.
             // We assume BAYER12_GRGB implies the format found in the file.
             return 8; 
        case JPEG_PIXEL_FORMAT_PACKED10:
        case JPEG_PIXEL_FORMAT_UNPACKED10:
            return 2; // 10-bit (1023) -> 8-bit (255) : >> 2
        case JPEG_PIXEL_FORMAT_UNPACKED16:
            return 8; // 16-bit (65535) -> 8-bit (255) : >> 8
        default:
            return 0; // Assume 8-bit
    }
}

// Calculate stride from source file
static int calculate_file_stride(int width, jpeg_pixel_format_t format) {
    switch (format) {
        case JPEG_PIXEL_FORMAT_PACKED10: return (width * 5) / 4; 
        case JPEG_PIXEL_FORMAT_PACKED12: return (width * 3) / 2; 
        case JPEG_PIXEL_FORMAT_UNPACKED8: return width;
        case JPEG_PIXEL_FORMAT_UNPACKED10:
        case JPEG_PIXEL_FORMAT_UNPACKED12:
        case JPEG_PIXEL_FORMAT_UNPACKED16:
        case JPEG_PIXEL_FORMAT_BAYER12_GRGB: return width * 2;
        default: return width;
    }
}

size_t jpeg_encoder_estimate_memory_requirement(const jpeg_encoder_config_t* config) {
    if (!config) return 0;
    int width = config->width;
    int file_stride = calculate_file_stride(width, config->pixel_format);

    int mcu_h = (config->subsample == JPEG_SUBSAMPLE_420) ? 16 : 8;
    int strip_lines = mcu_h + 2;
    int out_bpp = (config->subsample == JPEG_SUBSAMPLE_444) ? 3 : 2; // RGB888 or YUV422

    size_t sz_raw = file_stride * strip_lines;
    size_t sz_unpack = width * sizeof(uint16_t) * strip_lines;
    size_t sz_out = width * out_bpp * mcu_h;
    size_t sz_misc = (width * sizeof(uint16_t)) * 2; // carry_over + lookahead

    return sz_raw + sz_unpack + sz_out + sz_misc;
}

// Unpack one row of raw data into 16-bit buffer (keeping native range)
static void unpack_row(const uint8_t* src, uint16_t* dst, int width, jpeg_pixel_format_t format) {
    if (format == JPEG_PIXEL_FORMAT_UNPACKED16 || format == JPEG_PIXEL_FORMAT_BAYER12_GRGB) {
        // Direct copy, assumes Little Endian. 
        // For BAYER12_GRGB, we assume it's behaving like UNPACKED16 (MSB aligned) based on test file.
        memcpy(dst, src, width * sizeof(uint16_t));
    }
    else if (format == JPEG_PIXEL_FORMAT_UNPACKED12) {
        // 12-bit LSB aligned in 16-bit containers
        const uint16_t* s = (const uint16_t*)src;
        for (int i = 0; i < width; i++) dst[i] = s[i] & 0x0FFF;
    }
    else if (format == JPEG_PIXEL_FORMAT_UNPACKED10) {
         const uint16_t* s = (const uint16_t*)src;
         for (int i = 0; i < width; i++) dst[i] = s[i] & 0x03FF;
    }
    else if (format == JPEG_PIXEL_FORMAT_UNPACKED8) {
        for (int i = 0; i < width; i++) dst[i] = (uint16_t)src[i];
    }
    else if (format == JPEG_PIXEL_FORMAT_PACKED10) {
        int i_src = 0;
        for (int i = 0; i < width; i += 4) {
            uint8_t b0 = src[i_src++], b1 = src[i_src++], b2 = src[i_src++], b3 = src[i_src++], b4 = src[i_src++];
            if (i < width) dst[i]   = (b0 << 2) | ((b4 >> 0) & 0x03); 
            if (i+1 < width) dst[i+1] = (b1 << 2) | ((b4 >> 2) & 0x03);
            if (i+2 < width) dst[i+2] = (b2 << 2) | ((b4 >> 4) & 0x03);
            if (i+3 < width) dst[i+3] = (b3 << 2) | ((b4 >> 6) & 0x03);
        }
    }
    else if (format == JPEG_PIXEL_FORMAT_PACKED12) {
        int i_src = 0;
        for (int i = 0; i < width; i += 2) {
             uint8_t b0 = src[i_src++];
             uint8_t b1 = src[i_src++];
             uint8_t b2 = src[i_src++];
             if (i < width) dst[i] = ((uint16_t)b0 << 4) | (b2 & 0x0F);
             if (i+1 < width) dst[i+1] = ((uint16_t)b1 << 4) | (b2 >> 4);
        }
    }
}

static void subtract_black(uint16_t* row, int width, uint16_t ob) {
    if (ob == 0) return;
    for (int i = 0; i < width; i++) row[i] = (row[i] > ob) ? (row[i] - ob) : 0;
}

static void subtract_black_fast(uint16_t* row, int width, uint16_t ob) {
    if (ob == 0) return;
#if JPEG_ENC_HAS_DSP
    uint32_t ob2 = ((uint32_t)ob << 16) | ob;
    int i = 0;
    for (; i + 1 < width; i += 2) {
        uint32_t v = *(uint32_t *)&row[i];
        uint32_t r = __USUB16(v, ob2);
        *(uint32_t *)&row[i] = r;
    }
    if (i < width) {
        row[i] = (row[i] > ob) ? (row[i] - ob) : 0;
    }
#else
    for (int i = 0; i < width; i++) row[i] = (row[i] > ob) ? (row[i] - ob) : 0;
#endif
}

static inline int ob_adjust(uint16_t v, bool subtract_ob, uint16_t ob)
{
    if (!subtract_ob) return (int)v;
    return (v > ob) ? (int)(v - ob) : 0;
}

/* Macro version of ob_adjust for inlining in hot paths */
#define OB_ADJ(v, sub, ob) ((sub) ? (((v) > (ob)) ? ((int)(v) - (int)(ob)) : 0) : (int)(v))

/* FAST macro for inner loop when subtract_ob is known to be false at compile time.
 * Eliminates the conditional entirely - just casts to int. */
#define OB_ADJ_FAST(v) ((int)(v))

/* Combined gain+shift for demosaic: (val * gain_fix) >> (8 + shift_down) 
 * Pre-compute combined_shift = 8 + shift_down at loop start.
 * Use int64_t to avoid overflow for 16-bit Bayer * Q8 gain products.
 * On Cortex-M33, GCC emits SMULL (single-cycle 32x32→64) + register shift,
 * which is safe for the full value range and avoids silent truncation. */
#define APPLY_GAIN_SHIFT(val, gain, combined_shift) \
    ((int)(((int64_t)(val) * (int64_t)(gain)) >> (combined_shift)))

/* Demosaic algorithm selection:
 * 0 = Pure bilinear (fastest, slightly lower quality)
 * 1 = Gradient-corrected for R/B pixels only (good balance)
 * 2 = Full gradient correction (slower, best quality)
 */
#ifndef DEMOSAIC_USE_GRADIENT
#define DEMOSAIC_USE_GRADIENT 0  /* Default: pure bilinear for speed */
#endif

// JPEG Callbacks
static int32_t jpeg_write_callback(JPEGE_FILE *pFile, uint8_t *pBuf, int32_t iLen) {
    jpeg_stream_t* stream = (jpeg_stream_t*)pFile->fHandle;
    if (stream && stream->write) return (int32_t)stream->write(stream->write_ctx, pBuf, iLen);
    return 0;
}
static void* jpeg_open_callback(const char* szFilename) { return (void*)szFilename; }
static void jpeg_close_callback(JPEGE_FILE* pFile) { (void)pFile; }
static int32_t jpeg_read_callback(JPEGE_FILE* pFile, uint8_t* pBuf, int32_t iLen) { (void)pFile; (void)pBuf; (void)iLen; return 0; }
static int32_t jpeg_seek_callback(JPEGE_FILE* pFile, int32_t iPosition) { (void)pFile; (void)iPosition; return 0; }

// Demosaicing Helper: Fast Bilinear
// --- Reference Implementation ---
static void demosaic_row_bilinear_ref(
    const uint16_t* row_prev, 
    const uint16_t* row_curr, 
    const uint16_t* row_next, 
    uint8_t* rgb_out, 
    int width, 
    int y, 
    jpeg_bayer_pattern_t pattern, 
    float r_gain, 
    float b_gain,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value) 
{
    int row_phase = y & 1;
    
    for (int x = 0; x < width; x++) {
        int r = 0, g = 0, b = 0;
        int val = ob_adjust(row_curr[x], subtract_ob, ob_value);
        
        // 0=R, 1=G, 2=B
        int pixel_color = 1; 
        
        // Determine pixel color based on pattern and coordinates
        int is_even_y = (row_phase == 0);
        int is_even_x = ((x & 1) == 0);

        if (pattern == JPEG_BAYER_PATTERN_RGGB) {
            if (is_even_y) pixel_color = is_even_x ? 0 : 1; // R G
            else           pixel_color = is_even_x ? 1 : 2; // G B
        }
        else if (pattern == JPEG_BAYER_PATTERN_GRBG) {
            if (is_even_y) pixel_color = is_even_x ? 1 : 0; // G R
            else           pixel_color = is_even_x ? 2 : 1; // B G
        }
        else if (pattern == JPEG_BAYER_PATTERN_BGGR) {
            if (is_even_y) pixel_color = is_even_x ? 2 : 1; // B G
            else           pixel_color = is_even_x ? 1 : 0; // G R
        }
        else if (pattern == JPEG_BAYER_PATTERN_GBRG) {
            if (is_even_y) pixel_color = is_even_x ? 1 : 2; // G B
            else           pixel_color = is_even_x ? 0 : 1; // R G
        }
        else {
             // Fallback to GRBG (Previous Legacy Default)
            if (is_even_y) pixel_color = is_even_x ? 1 : 0; // G R
            else           pixel_color = is_even_x ? 2 : 1; // B G
        }
        
        int h_sum = 0, h_cnt = 0;
        int v_sum = 0, v_cnt = 0;
        int d_sum = 0, d_cnt = 0; 
        
        if (x > 0) { h_sum += ob_adjust(row_curr[x-1], subtract_ob, ob_value); h_cnt++; }
        if (x < width-1) { h_sum += ob_adjust(row_curr[x+1], subtract_ob, ob_value); h_cnt++; }
        
        if (row_prev) { v_sum += ob_adjust(row_prev[x], subtract_ob, ob_value); v_cnt++; }
        if (row_next) { v_sum += ob_adjust(row_next[x], subtract_ob, ob_value); v_cnt++; }
        
        if (row_prev && x > 0) { d_sum += ob_adjust(row_prev[x-1], subtract_ob, ob_value); d_cnt++; }
        if (row_prev && x < width-1) { d_sum += ob_adjust(row_prev[x+1], subtract_ob, ob_value); d_cnt++; }
        if (row_next && x > 0) { d_sum += ob_adjust(row_next[x-1], subtract_ob, ob_value); d_cnt++; }
        if (row_next && x < width-1) { d_sum += ob_adjust(row_next[x+1], subtract_ob, ob_value); d_cnt++; }

        if (pixel_color == 1) { // Green
            g = val;
            
            // Determine if current row contains Red pixels (vs Blue)
            // RGGB/GRBG have Red on even rows. BGGR/GBRG have Red on odd rows.
            int row_has_red = 0;
            if (pattern == JPEG_BAYER_PATTERN_BGGR || pattern == JPEG_BAYER_PATTERN_GBRG) {
                row_has_red = !is_even_y;
            } else {
                // RGGB, GRBG, and default (GRBG)
                row_has_red = is_even_y;
            }

            if (row_has_red) { // Neighbors: H->Red, V->Blue
                 if (h_cnt) r = h_sum / h_cnt;
                 if (v_cnt) b = v_sum / v_cnt;
            } else { // Neighbors: H->Blue, V->Red
                 if (h_cnt) b = h_sum / h_cnt;
                 if (v_cnt) r = v_sum / v_cnt;
            }
        }
        else if (pixel_color == 0) { // Red
            r = val;
            if (h_cnt + v_cnt > 0) g = (h_sum + v_sum) / (h_cnt + v_cnt);
            if (d_cnt) b = d_sum / d_cnt;
        }
        else { // Blue
            b = val;
            if (h_cnt + v_cnt > 0) g = (h_sum + v_sum) / (h_cnt + v_cnt);
            if (d_cnt) r = d_sum / d_cnt;
        }
        
        float r_f = (float)r * r_gain;
        float b_f = (float)b * b_gain;
        float g_f = (float)g;
        
        int r_i = (int)r_f;
        int g_i = (int)g_f;
        int b_i = (int)b_f;
        
        // Normalize
        r_i >>= shift_down;
        g_i >>= shift_down;
        b_i >>= shift_down;
        
        if (r_i > 255) r_i = 255;
        if (g_i > 255) g_i = 255;
        if (b_i > 255) b_i = 255;
        
        // Output BGR (Blue First) as per legacy implementation
        rgb_out[x*3 + 0] = (uint8_t)b_i;
        rgb_out[x*3 + 1] = (uint8_t)g_i;
        rgb_out[x*3 + 2] = (uint8_t)r_i;
    }
}

// --- Fast Implementation ---
static void demosaic_row_bilinear_fast(
    const uint16_t* restrict row_prev, 
    const uint16_t* restrict row_curr, 
    const uint16_t* restrict row_next, 
    uint8_t* restrict rgb_out, 
    int width, 
    int y, 
    jpeg_bayer_pattern_t pattern, 
    int r_gain_fix, 
    int b_gain_fix,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value) 
{
    int row_phase = y & 1;
    
    // We iterate entire width, but engage fast path only in middle
    for (int x = 0; x < width; x++) {
        
        // --- Fast Path Start ---
        if (row_prev && row_next && x > 0 && x < width - 1) {
            int val = ob_adjust(row_curr[x], subtract_ob, ob_value);
            int r,g,b;
            
            int p = ((int)pattern) & 3;
            int pixel_color = s_bayer_color_lut[p][row_phase][x & 1];
            int row_has_red = s_row_has_red_lut[p][row_phase];

            int h_sum = ob_adjust(row_curr[x-1], subtract_ob, ob_value) + ob_adjust(row_curr[x+1], subtract_ob, ob_value);
            int v_sum = ob_adjust(row_prev[x], subtract_ob, ob_value) + ob_adjust(row_next[x], subtract_ob, ob_value);
            
              if (pixel_color == 1) { // Green
                  if (row_has_red) { // H->Red, V->Blue
                     r = h_sum >> 1; b = v_sum >> 1;
                 } else { // H->Blue, V->Red
                     b = h_sum >> 1; r = v_sum >> 1;
                 }
                 g = val;
            } else if (pixel_color == 0) { // Red
                 r = val;
                 g = (h_sum + v_sum) >> 2;
                  b = (ob_adjust(row_prev[x-1], subtract_ob, ob_value) + ob_adjust(row_prev[x+1], subtract_ob, ob_value) +
                      ob_adjust(row_next[x-1], subtract_ob, ob_value) + ob_adjust(row_next[x+1], subtract_ob, ob_value)) >> 2;
            } else { // Blue
                 b = val;
                 g = (h_sum + v_sum) >> 2;
                  r = (ob_adjust(row_prev[x-1], subtract_ob, ob_value) + ob_adjust(row_prev[x+1], subtract_ob, ob_value) +
                      ob_adjust(row_next[x-1], subtract_ob, ob_value) + ob_adjust(row_next[x+1], subtract_ob, ob_value)) >> 2;
            }
            
            // Fixed Point Apply
            int r_i = (r * r_gain_fix) >> 8;
            int b_i = (b * b_gain_fix) >> 8;
            int g_i = (g * s_g_gain_fix) >> 8;
            
            r_i >>= shift_down;
            g_i >>= shift_down;
            b_i >>= shift_down;
            
            if (r_i > 255) r_i = 255;
            if (g_i > 255) g_i = 255;
            if (b_i > 255) b_i = 255;
            
            rgb_out[x*3] = (uint8_t)b_i;
            rgb_out[x*3+1] = (uint8_t)g_i;
            rgb_out[x*3+2] = (uint8_t)r_i;
            continue;
        }
        
        // Use Ref for edges to keep this simple
        // Note: Recursion? No, we call the _ref version explicitly.
        // But the ref version requires computing only one pixel?
        // Actually, _ref loops. We can just reimplement the edge logic here or call a single-pixel helper.
        // For simplicity, I'll just copy the edge logic fallback here or we can just run the REF loop for edges.
        // BUT, `demosaic_row_bilinear_ref` loops over the whole row. 
        // It's cleaner to just include the fallback logic here for edges.
        
        // --- Fallback (Slow) Path for Edges ---
        // (Copied from reference implementation for edge pixels)
        {
            int r=0,g=0,b=0;
            int val = ob_adjust(row_curr[x], subtract_ob, ob_value);
             // 0=R, 1=G, 2=B
            int p = ((int)pattern) & 3;
            int pixel_color = s_bayer_color_lut[p][row_phase][x & 1];
            int row_has_red = s_row_has_red_lut[p][row_phase];
            
            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0; 
            
            if (x > 0) { h_sum += ob_adjust(row_curr[x-1], subtract_ob, ob_value); h_cnt++; }
            if (x < width-1) { h_sum += ob_adjust(row_curr[x+1], subtract_ob, ob_value); h_cnt++; }
            
            if (row_prev) { v_sum += ob_adjust(row_prev[x], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += ob_adjust(row_next[x], subtract_ob, ob_value); v_cnt++; }
            
            if (row_prev && x > 0) { d_sum += ob_adjust(row_prev[x-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && x < width-1) { d_sum += ob_adjust(row_prev[x+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && x > 0) { d_sum += ob_adjust(row_next[x-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && x < width-1) { d_sum += ob_adjust(row_next[x+1], subtract_ob, ob_value); d_cnt++; }
    
            if (pixel_color == 1) { // Green
                g = val;
                if (row_has_red) { 
                     if (h_cnt) r = h_sum / h_cnt;
                     if (v_cnt) b = v_sum / v_cnt;
                } else { 
                     if (h_cnt) b = h_sum / h_cnt;
                     if (v_cnt) r = v_sum / v_cnt;
                }
            }
            else if (pixel_color == 0) { // Red
                r = val;
                if (h_cnt + v_cnt > 0) g = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b = d_sum / d_cnt;
            }
            else { // Blue
                b = val;
                if (h_cnt + v_cnt > 0) g = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r = d_sum / d_cnt;
            }
            
            int r_i = (r * r_gain_fix) >> 8;
            int b_i = (b * b_gain_fix) >> 8;
            int g_i = (g * s_g_gain_fix) >> 8;
            
            r_i >>= shift_down;
            g_i >>= shift_down;
            b_i >>= shift_down;
            
            if (r_i > 255) r_i = 255;
            if (g_i > 255) g_i = 255;
            if (b_i > 255) b_i = 255;
            
            rgb_out[x*3 + 0] = (uint8_t)b_i;
            rgb_out[x*3 + 1] = (uint8_t)g_i;
            rgb_out[x*3 + 2] = (uint8_t)r_i;
        }
    }
}

// --- Demosaic directly to YUV422 (slow reference) ---
static void demosaic_row_bilinear_to_yuv422_ref(
    const uint16_t* restrict row_prev,
    const uint16_t* restrict row_curr,
    const uint16_t* restrict row_next,
    uint8_t* restrict yuv_out,
    int width,
    int y,
    jpeg_bayer_pattern_t pattern,
    float r_gain,
    float b_gain,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value)
{
    int row_phase = y & 1;
    int x = 0;

    while (x < width)
    {
        int r0=0,g0=0,b0=0;
        int r1=0,g1=0,b1=0;

        // Pixel 0
        {
            int val = ob_adjust(row_curr[x], subtract_ob, ob_value);
            int p = ((int)pattern) & 3;
            int pixel_color = s_bayer_color_lut[p][row_phase][x & 1];
            int row_has_red = s_row_has_red_lut[p][row_phase];

            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;

            if (x > 0) { h_sum += ob_adjust(row_curr[x-1], subtract_ob, ob_value); h_cnt++; }
            if (x < width-1) { h_sum += ob_adjust(row_curr[x+1], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += ob_adjust(row_prev[x], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += ob_adjust(row_next[x], subtract_ob, ob_value); v_cnt++; }
            if (row_prev && x > 0) { d_sum += ob_adjust(row_prev[x-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && x < width-1) { d_sum += ob_adjust(row_prev[x+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && x > 0) { d_sum += ob_adjust(row_next[x-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && x < width-1) { d_sum += ob_adjust(row_next[x+1], subtract_ob, ob_value); d_cnt++; }

            if (pixel_color == 1) {
                g0 = val;
                if (row_has_red) {
                    if (h_cnt) r0 = h_sum / h_cnt;
                    if (v_cnt) b0 = v_sum / v_cnt;
                } else {
                    if (h_cnt) b0 = h_sum / h_cnt;
                    if (v_cnt) r0 = v_sum / v_cnt;
                }
            }
            else if (pixel_color == 0) {
                r0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b0 = d_sum / d_cnt;
            }
            else {
                b0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r0 = d_sum / d_cnt;
            }
        }

        // Pixel 1 (or duplicate if odd width)
        if (x + 1 < width)
        {
            int val = ob_adjust(row_curr[x + 1], subtract_ob, ob_value);
            int p = ((int)pattern) & 3;
            int pixel_color = s_bayer_color_lut[p][row_phase][(x + 1) & 1];
            int row_has_red = s_row_has_red_lut[p][row_phase];

            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;
            int xi = x + 1;

            if (xi > 0) { h_sum += ob_adjust(row_curr[xi-1], subtract_ob, ob_value); h_cnt++; }
            if (xi < width-1) { h_sum += ob_adjust(row_curr[xi+1], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += ob_adjust(row_prev[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += ob_adjust(row_next[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_prev && xi > 0) { d_sum += ob_adjust(row_prev[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && xi < width-1) { d_sum += ob_adjust(row_prev[xi+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi > 0) { d_sum += ob_adjust(row_next[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi < width-1) { d_sum += ob_adjust(row_next[xi+1], subtract_ob, ob_value); d_cnt++; }

            if (pixel_color == 1) {
                g1 = val;
                if (row_has_red) {
                    if (h_cnt) r1 = h_sum / h_cnt;
                    if (v_cnt) b1 = v_sum / v_cnt;
                } else {
                    if (h_cnt) b1 = h_sum / h_cnt;
                    if (v_cnt) r1 = v_sum / v_cnt;
                }
            }
            else if (pixel_color == 0) {
                r1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b1 = d_sum / d_cnt;
            }
            else {
                b1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r1 = d_sum / d_cnt;
            }
        }
        else {
            r1 = r0; g1 = g0; b1 = b0;
        }

        // Apply gains + normalize to 8-bit
        int r0_i = (int)((float)r0 * r_gain);
        int g0_i = (int)((float)g0 * s_g_gain);
        int b0_i = (int)((float)b0 * b_gain);
        int r1_i = (int)((float)r1 * r_gain);
        int g1_i = (int)((float)g1 * s_g_gain);
        int b1_i = (int)((float)b1 * b_gain);

        r0_i >>= shift_down; g0_i >>= shift_down; b0_i >>= shift_down;
        r1_i >>= shift_down; g1_i >>= shift_down; b1_i >>= shift_down;

        if (r0_i > 255) r0_i = 255; if (g0_i > 255) g0_i = 255; if (b0_i > 255) b0_i = 255;
        if (r1_i > 255) r1_i = 255; if (g1_i > 255) g1_i = 255; if (b1_i > 255) b1_i = 255;

        // RGB -> YCbCr (unsigned)
        int y0 = (((r0_i * 1225) + (g0_i * 2404) + (b0_i * 467)) >> 12);
        int y1 = (((r1_i * 1225) + (g1_i * 2404) + (b1_i * 467)) >> 12);
        int cb0 = ((b0_i << 11) + (r0_i * -691) + (g0_i * -1357)) >> 12;
        int cr0 = ((r0_i << 11) + (g0_i * -1715) + (b0_i * -333)) >> 12;
        int cb1 = ((b1_i << 11) + (r1_i * -691) + (g1_i * -1357)) >> 12;
        int cr1 = ((r1_i << 11) + (g1_i * -1715) + (b1_i * -333)) >> 12;

        int cb = ((cb0 + cb1) >> 1) + 128;
        int cr = ((cr0 + cr1) >> 1) + 128;
        cb = clamp_u8(cb);
        cr = clamp_u8(cr);

        if (y0 < 0) y0 = 0; else if (y0 > 255) y0 = 255;
        if (y1 < 0) y1 = 0; else if (y1 > 255) y1 = 255;
        y0 = s_y_lut[y0];
        y1 = s_y_lut[y1];

        // Pack Y0 Cb Y1 Cr (YUYV)
        int out_idx = x * 2;
        yuv_out[out_idx + 0] = (uint8_t)y0;
        yuv_out[out_idx + 1] = (uint8_t)cb;
        yuv_out[out_idx + 2] = (uint8_t)y1;
        yuv_out[out_idx + 3] = (uint8_t)cr;

        x += 2;
    }
}

// --- Demosaic directly to YUV444 (slow reference) ---
static void demosaic_row_bilinear_to_yuv444_ref(
    const uint16_t* restrict row_prev,
    const uint16_t* restrict row_curr,
    const uint16_t* restrict row_next,
    uint8_t* restrict yuv_out,
    int width,
    int y,
    jpeg_bayer_pattern_t pattern,
    float r_gain,
    float b_gain,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value)
{
    int row_phase = y & 1;
    int x = 0;

    while (x < width)
    {
        int r0=0,g0=0,b0=0;
        int r1=0,g1=0,b1=0;

        // Pixel 0
        {
            int val = ob_adjust(row_curr[x], subtract_ob, ob_value);
            int p = ((int)pattern) & 3;
            int pixel_color = s_bayer_color_lut[p][row_phase][x & 1];
            int row_has_red = s_row_has_red_lut[p][row_phase];

            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;

            if (x > 0) { h_sum += ob_adjust(row_curr[x-1], subtract_ob, ob_value); h_cnt++; }
            if (x < width-1) { h_sum += ob_adjust(row_curr[x+1], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += ob_adjust(row_prev[x], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += ob_adjust(row_next[x], subtract_ob, ob_value); v_cnt++; }
            if (row_prev && x > 0) { d_sum += ob_adjust(row_prev[x-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && x < width-1) { d_sum += ob_adjust(row_prev[x+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && x > 0) { d_sum += ob_adjust(row_next[x-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && x < width-1) { d_sum += ob_adjust(row_next[x+1], subtract_ob, ob_value); d_cnt++; }

            if (pixel_color == 1) {
                g0 = val;
                if (row_has_red) {
                    if (h_cnt) r0 = h_sum / h_cnt;
                    if (v_cnt) b0 = v_sum / v_cnt;
                } else {
                    if (h_cnt) b0 = h_sum / h_cnt;
                    if (v_cnt) r0 = v_sum / v_cnt;
                }
            }
            else if (pixel_color == 0) {
                r0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b0 = d_sum / d_cnt;
            }
            else {
                b0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r0 = d_sum / d_cnt;
            }
        }

        // Pixel 1 (or duplicate if odd width)
        if (x + 1 < width)
        {
            int val = ob_adjust(row_curr[x + 1], subtract_ob, ob_value);
            int p = ((int)pattern) & 3;
            int pixel_color = s_bayer_color_lut[p][row_phase][(x + 1) & 1];
            int row_has_red = s_row_has_red_lut[p][row_phase];

            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;
            int xi = x + 1;

            if (xi > 0) { h_sum += ob_adjust(row_curr[xi-1], subtract_ob, ob_value); h_cnt++; }
            if (xi < width-1) { h_sum += ob_adjust(row_curr[xi+1], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += ob_adjust(row_prev[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += ob_adjust(row_next[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_prev && xi > 0) { d_sum += ob_adjust(row_prev[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && xi < width-1) { d_sum += ob_adjust(row_prev[xi+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi > 0) { d_sum += ob_adjust(row_next[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi < width-1) { d_sum += ob_adjust(row_next[xi+1], subtract_ob, ob_value); d_cnt++; }

            if (pixel_color == 1) {
                g1 = val;
                if (row_has_red) {
                    if (h_cnt) r1 = h_sum / h_cnt;
                    if (v_cnt) b1 = v_sum / v_cnt;
                } else {
                    if (h_cnt) b1 = h_sum / h_cnt;
                    if (v_cnt) r1 = v_sum / v_cnt;
                }
            }
            else if (pixel_color == 0) {
                r1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b1 = d_sum / d_cnt;
            }
            else {
                b1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r1 = d_sum / d_cnt;
            }
        }
        else {
            r1 = r0; g1 = g0; b1 = b0;
        }

        int r0_i = (int)((float)r0 * r_gain);
        int g0_i = (int)((float)g0 * s_g_gain);
        int b0_i = (int)((float)b0 * b_gain);
        int r1_i = (int)((float)r1 * r_gain);
        int g1_i = (int)((float)g1 * s_g_gain);
        int b1_i = (int)((float)b1 * b_gain);

        r0_i >>= shift_down; g0_i >>= shift_down; b0_i >>= shift_down;
        r1_i >>= shift_down; g1_i >>= shift_down; b1_i >>= shift_down;

        if (r0_i > 255) r0_i = 255; if (g0_i > 255) g0_i = 255; if (b0_i > 255) b0_i = 255;
        if (r1_i > 255) r1_i = 255; if (g1_i > 255) g1_i = 255; if (b1_i > 255) b1_i = 255;

        int y0 = (((r0_i * 1225) + (g0_i * 2404) + (b0_i * 467)) >> 12);
        int cb0 = ((b0_i << 11) + (r0_i * -691) + (g0_i * -1357)) >> 12;
        int cr0 = ((r0_i << 11) + (g0_i * -1715) + (b0_i * -333)) >> 12;
        int y1 = (((r1_i * 1225) + (g1_i * 2404) + (b1_i * 467)) >> 12);
        int cb1 = ((b1_i << 11) + (r1_i * -691) + (g1_i * -1357)) >> 12;
        int cr1 = ((r1_i << 11) + (g1_i * -1715) + (b1_i * -333)) >> 12;

        cb0 += 128; cr0 += 128; cb1 += 128; cr1 += 128;
        if (y0 < 0) y0 = 0; else if (y0 > 255) y0 = 255;
        if (y1 < 0) y1 = 0; else if (y1 > 255) y1 = 255;
        y0 = s_y_lut[y0];
        y1 = s_y_lut[y1];
        cb0 = clamp_u8(cb0);
        cr0 = clamp_u8(cr0);
        cb1 = clamp_u8(cb1);
        cr1 = clamp_u8(cr1);

        int out_idx = x * 3;
        yuv_out[out_idx + 0] = (uint8_t)y0;
        yuv_out[out_idx + 1] = (uint8_t)cb0;
        yuv_out[out_idx + 2] = (uint8_t)cr0;
        if (x + 1 < width) {
            yuv_out[out_idx + 3] = (uint8_t)y1;
            yuv_out[out_idx + 4] = (uint8_t)cb1;
            yuv_out[out_idx + 5] = (uint8_t)cr1;
        }

        x += 2;
    }
}

// --- Demosaic directly to YUV422 (fast path) ---
// OPTIMIZED: Unrolled inner loop, macro for ob_adjust, separated edge handling
__attribute__((hot))
static void demosaic_row_bilinear_to_yuv422_fast(
    const uint16_t* restrict row_prev,
    const uint16_t* restrict row_curr,
    const uint16_t* restrict row_next,
    uint8_t* restrict yuv_out,
    int width,
    int y,
    jpeg_bayer_pattern_t pattern,
    int r_gain_fix,
    int b_gain_fix,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value)
{
    const int row_phase = y & 1;
    const int p = ((int)pattern) & 3;
    
    /* Precompute row-level pattern values (same for all x) */
    const int row_has_red = s_row_has_red_lut[p][row_phase];
    const uint8_t *color_lut_row = s_bayer_color_lut[p][row_phase];
    
    /* Check if we have valid prev/next rows (needed for fast path) */
    const int have_full_rows = (row_prev != NULL && row_next != NULL);
    
    int x = 0;
    
    /* --- Process first pixel pair (x=0,1) with edge handling --- */
    if (width >= 2)
    {
        int r0=0,g0=0,b0=0;
        int r1=0,g1=0,b1=0;
        
        /* Pixel 0 (x=0): left edge, need fallback */
        {
            int val = OB_ADJ(row_curr[0], subtract_ob, ob_value);
            int pixel_color = color_lut_row[0];
            
            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;
            
            /* x=0: no left neighbor, has right neighbor */
            h_sum = OB_ADJ(row_curr[1], subtract_ob, ob_value); h_cnt = 1;
            if (row_prev) { v_sum += OB_ADJ(row_prev[0], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += OB_ADJ(row_next[0], subtract_ob, ob_value); v_cnt++; }
            if (row_prev) { d_sum += OB_ADJ(row_prev[1], subtract_ob, ob_value); d_cnt++; }
            if (row_next) { d_sum += OB_ADJ(row_next[1], subtract_ob, ob_value); d_cnt++; }
            
            if (pixel_color == 1) {
                g0 = val;
                if (row_has_red) { if (h_cnt) r0 = h_sum / h_cnt; if (v_cnt) b0 = v_sum / v_cnt; }
                else             { if (h_cnt) b0 = h_sum / h_cnt; if (v_cnt) r0 = v_sum / v_cnt; }
            } else if (pixel_color == 0) {
                r0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b0 = d_sum / d_cnt;
            } else {
                b0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r0 = d_sum / d_cnt;
            }
        }
        
        /* Pixel 1 (x=1): has both neighbors if prev/next exist */
        if (have_full_rows && width > 2)
        {
            /* Fast path for x=1 */
            int val = OB_ADJ(row_curr[1], subtract_ob, ob_value);
            int pixel_color = color_lut_row[1];
            
            int h_sum = OB_ADJ(row_curr[0], subtract_ob, ob_value) + OB_ADJ(row_curr[2], subtract_ob, ob_value);
            int v_sum = OB_ADJ(row_prev[1], subtract_ob, ob_value) + OB_ADJ(row_next[1], subtract_ob, ob_value);
            
            if (pixel_color == 1) {
                if (row_has_red) { r1 = h_sum >> 1; b1 = v_sum >> 1; }
                else             { b1 = h_sum >> 1; r1 = v_sum >> 1; }
                g1 = val;
            } else if (pixel_color == 0) {
                r1 = val;
                g1 = (h_sum + v_sum) >> 2;
                b1 = (OB_ADJ(row_prev[0], subtract_ob, ob_value) + OB_ADJ(row_prev[2], subtract_ob, ob_value) +
                      OB_ADJ(row_next[0], subtract_ob, ob_value) + OB_ADJ(row_next[2], subtract_ob, ob_value)) >> 2;
            } else {
                b1 = val;
                g1 = (h_sum + v_sum) >> 2;
                r1 = (OB_ADJ(row_prev[0], subtract_ob, ob_value) + OB_ADJ(row_prev[2], subtract_ob, ob_value) +
                      OB_ADJ(row_next[0], subtract_ob, ob_value) + OB_ADJ(row_next[2], subtract_ob, ob_value)) >> 2;
            }
        }
        else
        {
            /* Fallback for x=1 */
            int val = OB_ADJ(row_curr[1], subtract_ob, ob_value);
            int pixel_color = color_lut_row[1];
            
            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;
            
            h_sum = OB_ADJ(row_curr[0], subtract_ob, ob_value); h_cnt++;
            if (width > 2) { h_sum += OB_ADJ(row_curr[2], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += OB_ADJ(row_prev[1], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += OB_ADJ(row_next[1], subtract_ob, ob_value); v_cnt++; }
            if (row_prev) { d_sum += OB_ADJ(row_prev[0], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && width > 2) { d_sum += OB_ADJ(row_prev[2], subtract_ob, ob_value); d_cnt++; }
            if (row_next) { d_sum += OB_ADJ(row_next[0], subtract_ob, ob_value); d_cnt++; }
            if (row_next && width > 2) { d_sum += OB_ADJ(row_next[2], subtract_ob, ob_value); d_cnt++; }
            
            if (pixel_color == 1) {
                g1 = val;
                if (row_has_red) { if (h_cnt) r1 = h_sum / h_cnt; if (v_cnt) b1 = v_sum / v_cnt; }
                else             { if (h_cnt) b1 = h_sum / h_cnt; if (v_cnt) r1 = v_sum / v_cnt; }
            } else if (pixel_color == 0) {
                r1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b1 = d_sum / d_cnt;
            } else {
                b1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r1 = d_sum / d_cnt;
            }
        }
        
        /* Apply gains and convert to YUV */
        int r0_i = (r0 * r_gain_fix) >> 8;
        int b0_i = (b0 * b_gain_fix) >> 8;
        int g0_i = (g0 * s_g_gain_fix) >> 8;
        int r1_i = (r1 * r_gain_fix) >> 8;
        int b1_i = (b1 * b_gain_fix) >> 8;
        int g1_i = (g1 * s_g_gain_fix) >> 8;
        
        r0_i >>= shift_down; g0_i >>= shift_down; b0_i >>= shift_down;
        r1_i >>= shift_down; g1_i >>= shift_down; b1_i >>= shift_down;
        
        CLAMP_SAT(r0_i); CLAMP_SAT(g0_i); CLAMP_SAT(b0_i);
        CLAMP_SAT(r1_i); CLAMP_SAT(g1_i); CLAMP_SAT(b1_i);
        
        int rg0 = JPEG_ENC_PACK16(r0_i, g0_i);
        int rg1 = JPEG_ENC_PACK16(r1_i, g1_i);
        
        int y0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_Y_RG, b0_i * JPEG_ENC_COEF_Y_B) >> 12;
        int y1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_Y_RG, b1_i * JPEG_ENC_COEF_Y_B) >> 12;
        int cb0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CB_RG, b0_i << 11) >> 12;
        int cr0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CR_RG, b0_i * JPEG_ENC_COEF_CR_B) >> 12;
        int cb1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CB_RG, b1_i << 11) >> 12;
        int cr1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CR_RG, b1_i * JPEG_ENC_COEF_CR_B) >> 12;
        
        int cb = ((cb0 + cb1) >> 1) + 128;
        int cr = ((cr0 + cr1) >> 1) + 128;
        cb = clamp_u8(cb);
        cr = clamp_u8(cr);
        
        CLAMP_SAT(y0); CLAMP_SAT(y1);
        y0 = s_y_lut[y0];
        y1 = s_y_lut[y1];
        
        yuv_out[0] = (uint8_t)y0;
        yuv_out[1] = (uint8_t)cb;
        yuv_out[2] = (uint8_t)y1;
        yuv_out[3] = (uint8_t)cr;
        
        x = 2;
    }
    
    /* --- MAIN LOOP: Middle pixels (x=2 to width-4) - NO edge checks --- */
    /* OPTIMIZED: 2-pixel processing with pointer-based access */
    if (have_full_rows)
    {
        const int x_end = (width - 2) & ~1;  /* Round down to even */
        const int color0 = color_lut_row[0];
        const int color1 = color_lut_row[1];
        const int combined_shift = 8 + shift_down;
        
        /* Pointer-based output to avoid index multiply */
        uint8_t *out_ptr = &yuv_out[x * 2];
        
        for (; x < x_end; x += 2)
        {
            /* Pointer-based input access */
            const uint16_t *curr = &row_curr[x];
            const uint16_t *prev = &row_prev[x];
            const uint16_t *next = &row_next[x];
            
            /* Load current row pixels (always needed individually) */
            const int c_m1 = OB_ADJ_FAST(curr[-1]);
            const int c_0  = OB_ADJ_FAST(curr[0]);
            const int c_1  = OB_ADJ_FAST(curr[1]);
            const int c_2  = OB_ADJ_FAST(curr[2]);
            
            /* Precompute vertical sums: reused across pixel 0 and pixel 1 */
            /* UHADD16 computes unsigned halving add of packed halfwords: */
            /*   result[15:0]  = (op1[15:0]  + op2[15:0])  >> 1           */
            /*   result[31:16] = (op1[31:16] + op2[31:16]) >> 1           */
            /* This is overflow-safe (17-bit intermediate) and gives the  */
            /* exact 2-way average we need for bilinear interpolation.    */
#if JPEG_ENC_HAS_DSP
            /* Packed 32-bit loads: each gets two adjacent uint16_t values */
            const uint32_t _pp01 = *(const uint32_t *)prev; /* {prev[x], prev[x+1]} */
            const uint32_t _np01 = *(const uint32_t *)next; /* {next[x], next[x+1]} */
            /* Halving adds for overflow-safe vertical averages */
            const uint32_t _vhalf01 = __UHADD16(_pp01, _np01);
            const int vhalf_0 = (int)(uint16_t)_vhalf01;        /* (p[0]+n[0])/2 */
            const int vhalf_1 = (int)(uint16_t)(_vhalf01 >> 16); /* (p[1]+n[1])/2 */
            /* Full vertical sums (32-bit, no overflow) for 4-way averages */
            const int p_0 = (int)(uint16_t)_pp01;
            const int p_1 = (int)(uint16_t)(_pp01 >> 16);
            const int n_0 = (int)(uint16_t)_np01;
            const int n_1 = (int)(uint16_t)(_np01 >> 16);
#else
            const int p_0  = OB_ADJ_FAST(prev[0]);
            const int p_1  = OB_ADJ_FAST(prev[1]);
            const int n_0  = OB_ADJ_FAST(next[0]);
            const int n_1  = OB_ADJ_FAST(next[1]);
            const int vhalf_0 = (p_0 + n_0) >> 1;
            const int vhalf_1 = (p_1 + n_1) >> 1;
#endif
            /* Remaining edge pixels (individual loads) */
            const int p_m1 = OB_ADJ_FAST(prev[-1]);
            const int p_2  = OB_ADJ_FAST(prev[2]);
            const int n_m1 = OB_ADJ_FAST(next[-1]);
            const int n_2  = OB_ADJ_FAST(next[2]);
            
            /* Precomputed sums reused across both pixels */
            const int vsum_0 = p_0 + n_0;  /* vertical sum at x   */
            const int vsum_1 = p_1 + n_1;  /* vertical sum at x+1 */
            
            int r0, g0, b0, r1, g1, b1;
            
            /* Pixel 0 (even position) */
            {
                const int h_sum = c_m1 + c_1;
                if (color0 == 1) {
                    g0 = c_0;
                    /* Use UHADD16-derived halving adds for 2-way averages */
                    if (row_has_red) { r0 = h_sum >> 1; b0 = vhalf_0; }
                    else             { b0 = h_sum >> 1; r0 = vhalf_0; }
                } else if (color0 == 0) {
                    r0 = c_0;
#if DEMOSAIC_USE_GRADIENT >= 1
                    const int d_sum = p_m1 + p_1 + n_m1 + n_1;
                    const int lap8 = ((c_0 << 2) - d_sum) >> 3;
                    g0 = ((h_sum + vsum_0) >> 2) + lap8;
                    b0 = (d_sum >> 2) + lap8;
#else
                    g0 = (h_sum + vsum_0) >> 2;
                    b0 = (p_m1 + n_m1 + vsum_1) >> 2;
#endif
                } else {
                    b0 = c_0;
#if DEMOSAIC_USE_GRADIENT >= 1
                    const int d_sum = p_m1 + p_1 + n_m1 + n_1;
                    const int lap8 = ((c_0 << 2) - d_sum) >> 3;
                    g0 = ((h_sum + vsum_0) >> 2) + lap8;
                    r0 = (d_sum >> 2) + lap8;
#else
                    g0 = (h_sum + vsum_0) >> 2;
                    r0 = (p_m1 + n_m1 + vsum_1) >> 2;
#endif
                }
            }
            
            /* Pixel 1 (odd position) */
            {
                const int h_sum = c_0 + c_2;
                if (color1 == 1) {
                    g1 = c_1;
                    /* Use UHADD16-derived halving adds for 2-way averages */
                    if (row_has_red) { r1 = h_sum >> 1; b1 = vhalf_1; }
                    else             { b1 = h_sum >> 1; r1 = vhalf_1; }
                } else if (color1 == 0) {
                    r1 = c_1;
#if DEMOSAIC_USE_GRADIENT >= 1
                    const int d_sum = p_0 + p_2 + n_0 + n_2;
                    const int lap8 = ((c_1 << 2) - d_sum) >> 3;
                    g1 = ((h_sum + vsum_1) >> 2) + lap8;
                    b1 = (d_sum >> 2) + lap8;
#else
                    g1 = (h_sum + vsum_1) >> 2;
                    b1 = (vsum_0 + p_2 + n_2) >> 2;
#endif
                } else {
                    b1 = c_1;
#if DEMOSAIC_USE_GRADIENT >= 1
                    const int d_sum = p_0 + p_2 + n_0 + n_2;
                    const int lap8 = ((c_1 << 2) - d_sum) >> 3;
                    g1 = ((h_sum + vsum_1) >> 2) + lap8;
                    r1 = (d_sum >> 2) + lap8;
#else
                    g1 = (h_sum + vsum_1) >> 2;
                    r1 = (vsum_0 + p_2 + n_2) >> 2;
#endif
                }
            }
            
            /* Apply gains */
            int r0_i = APPLY_GAIN_SHIFT(r0, r_gain_fix, combined_shift);
            int g0_i = APPLY_GAIN_SHIFT(g0, s_g_gain_fix, combined_shift);
            int b0_i = APPLY_GAIN_SHIFT(b0, b_gain_fix, combined_shift);
            int r1_i = APPLY_GAIN_SHIFT(r1, r_gain_fix, combined_shift);
            int g1_i = APPLY_GAIN_SHIFT(g1, s_g_gain_fix, combined_shift);
            int b1_i = APPLY_GAIN_SHIFT(b1, b_gain_fix, combined_shift);
            CLAMP_SAT(r0_i); CLAMP_SAT(g0_i); CLAMP_SAT(b0_i);
            CLAMP_SAT(r1_i); CLAMP_SAT(g1_i); CLAMP_SAT(b1_i);
            
            /* YUV conversion with merged Cb/Cr averaging */
            int rg0 = JPEG_ENC_PACK16(r0_i, g0_i);
            int rg1 = JPEG_ENC_PACK16(r1_i, g1_i);
            int y0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_Y_RG, b0_i * JPEG_ENC_COEF_Y_B) >> 12;
            int y1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_Y_RG, b1_i * JPEG_ENC_COEF_Y_B) >> 12;
            int cb = ((JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CB_RG, b0_i << 11) +
                       JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CB_RG, b1_i << 11)) >> 13) + 128;
            int cr = ((JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CR_RG, b0_i * JPEG_ENC_COEF_CR_B) +
                       JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CR_RG, b1_i * JPEG_ENC_COEF_CR_B)) >> 13) + 128;
            CLAMP_SAT(y0); CLAMP_SAT(y1);
            
            /* Pointer-based output */
            out_ptr[0] = s_y_lut[y0];
            out_ptr[1] = (uint8_t)clamp_u8(cb);
            out_ptr[2] = s_y_lut[y1];
            out_ptr[3] = (uint8_t)clamp_u8(cr);
            out_ptr += 4;
        }
    }
    
    /* --- Process remaining pixel pairs (last 1-2 pairs with edge handling) --- */
    for (; x < width; x += 2)
    {
        int r0=0,g0=0,b0=0;
        int r1=0,g1=0,b1=0;

        /* Pixel 0 */
        {
            int xi = x;
            int val = OB_ADJ(row_curr[xi], subtract_ob, ob_value);
            int pixel_color = color_lut_row[xi & 1];

            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;

            if (xi > 0) { h_sum += OB_ADJ(row_curr[xi-1], subtract_ob, ob_value); h_cnt++; }
            if (xi < width-1) { h_sum += OB_ADJ(row_curr[xi+1], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += OB_ADJ(row_prev[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += OB_ADJ(row_next[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_prev && xi > 0) { d_sum += OB_ADJ(row_prev[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && xi < width-1) { d_sum += OB_ADJ(row_prev[xi+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi > 0) { d_sum += OB_ADJ(row_next[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi < width-1) { d_sum += OB_ADJ(row_next[xi+1], subtract_ob, ob_value); d_cnt++; }

            if (pixel_color == 1) {
                g0 = val;
                if (row_has_red) { if (h_cnt) r0 = h_sum / h_cnt; if (v_cnt) b0 = v_sum / v_cnt; }
                else             { if (h_cnt) b0 = h_sum / h_cnt; if (v_cnt) r0 = v_sum / v_cnt; }
            } else if (pixel_color == 0) {
                r0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b0 = d_sum / d_cnt;
            } else {
                b0 = val;
                if (h_cnt + v_cnt > 0) g0 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r0 = d_sum / d_cnt;
            }
        }

        /* Pixel 1 */
        if (x + 1 < width)
        {
            int xi = x + 1;
            int val = OB_ADJ(row_curr[xi], subtract_ob, ob_value);
            int pixel_color = color_lut_row[xi & 1];

            int h_sum = 0, h_cnt = 0;
            int v_sum = 0, v_cnt = 0;
            int d_sum = 0, d_cnt = 0;

            if (xi > 0) { h_sum += OB_ADJ(row_curr[xi-1], subtract_ob, ob_value); h_cnt++; }
            if (xi < width-1) { h_sum += OB_ADJ(row_curr[xi+1], subtract_ob, ob_value); h_cnt++; }
            if (row_prev) { v_sum += OB_ADJ(row_prev[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_next) { v_sum += OB_ADJ(row_next[xi], subtract_ob, ob_value); v_cnt++; }
            if (row_prev && xi > 0) { d_sum += OB_ADJ(row_prev[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_prev && xi < width-1) { d_sum += OB_ADJ(row_prev[xi+1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi > 0) { d_sum += OB_ADJ(row_next[xi-1], subtract_ob, ob_value); d_cnt++; }
            if (row_next && xi < width-1) { d_sum += OB_ADJ(row_next[xi+1], subtract_ob, ob_value); d_cnt++; }

            if (pixel_color == 1) {
                g1 = val;
                if (row_has_red) { if (h_cnt) r1 = h_sum / h_cnt; if (v_cnt) b1 = v_sum / v_cnt; }
                else             { if (h_cnt) b1 = h_sum / h_cnt; if (v_cnt) r1 = v_sum / v_cnt; }
            } else if (pixel_color == 0) {
                r1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) b1 = d_sum / d_cnt;
            } else {
                b1 = val;
                if (h_cnt + v_cnt > 0) g1 = (h_sum + v_sum) / (h_cnt + v_cnt);
                if (d_cnt) r1 = d_sum / d_cnt;
            }
        }
        else
        {
            r1 = r0; g1 = g0; b1 = b0;
        }

        int r0_i = (r0 * r_gain_fix) >> 8;
        int b0_i = (b0 * b_gain_fix) >> 8;
        int g0_i = (g0 * s_g_gain_fix) >> 8;
        int r1_i = (r1 * r_gain_fix) >> 8;
        int b1_i = (b1 * b_gain_fix) >> 8;
        int g1_i = (g1 * s_g_gain_fix) >> 8;

        r0_i >>= shift_down; g0_i >>= shift_down; b0_i >>= shift_down;
        r1_i >>= shift_down; g1_i >>= shift_down; b1_i >>= shift_down;

        CLAMP_SAT(r0_i); CLAMP_SAT(g0_i); CLAMP_SAT(b0_i);
        CLAMP_SAT(r1_i); CLAMP_SAT(g1_i); CLAMP_SAT(b1_i);

        int rg0 = JPEG_ENC_PACK16(r0_i, g0_i);
        int rg1 = JPEG_ENC_PACK16(r1_i, g1_i);

        int y0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_Y_RG, b0_i * JPEG_ENC_COEF_Y_B) >> 12;
        int y1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_Y_RG, b1_i * JPEG_ENC_COEF_Y_B) >> 12;
        int cb0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CB_RG, b0_i << 11) >> 12;
        int cr0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CR_RG, b0_i * JPEG_ENC_COEF_CR_B) >> 12;
        int cb1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CB_RG, b1_i << 11) >> 12;
        int cr1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CR_RG, b1_i * JPEG_ENC_COEF_CR_B) >> 12;

        int cb = ((cb0 + cb1) >> 1) + 128;
        int cr = ((cr0 + cr1) >> 1) + 128;
        cb = clamp_u8(cb);
        cr = clamp_u8(cr);

        CLAMP_SAT(y0); CLAMP_SAT(y1);
        y0 = s_y_lut[y0];
        y1 = s_y_lut[y1];

        int out_idx = x * 2;
        yuv_out[out_idx + 0] = (uint8_t)y0;
        yuv_out[out_idx + 1] = (uint8_t)cb;
        yuv_out[out_idx + 2] = (uint8_t)y1;
        yuv_out[out_idx + 3] = (uint8_t)cr;
    }
}

// --- Demosaic to YUV422 luma only (fast path) ---
// Writes Y for each pixel and leaves chroma untouched for caller to fill.
__attribute__((hot))
static void demosaic_row_bilinear_to_yuv422_luma_fast(
    const uint16_t* restrict row_prev,
    const uint16_t* restrict row_curr,
    const uint16_t* restrict row_next,
    uint8_t* restrict yuv_out,
    int width,
    int y,
    jpeg_bayer_pattern_t pattern,
    int r_gain_fix,
    int b_gain_fix,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value)
{
    int row_phase = y & 1;

    for (int x = 0; x < width; x += 2)
    {
        int r0=0,g0=0,b0=0;
        int r1=0,g1=0,b1=0;

        for (int i = 0; i < 2; i++)
        {
            int xi = x + i;
            int *r = (i == 0) ? &r0 : &r1;
            int *g = (i == 0) ? &g0 : &g1;
            int *b = (i == 0) ? &b0 : &b1;

            if (xi >= width) {
                *r = r0; *g = g0; *b = b0;
                continue;
            }

            if (row_prev && row_next && xi > 0 && xi < width - 1) {
                int val = ob_adjust(row_curr[xi], subtract_ob, ob_value);
                int p = ((int)pattern) & 3;
                int pixel_color = s_bayer_color_lut[p][row_phase][xi & 1];
                int row_has_red = s_row_has_red_lut[p][row_phase];

                int h_sum = ob_adjust(row_curr[xi-1], subtract_ob, ob_value) + ob_adjust(row_curr[xi+1], subtract_ob, ob_value);
                int v_sum = ob_adjust(row_prev[xi], subtract_ob, ob_value) + ob_adjust(row_next[xi], subtract_ob, ob_value);

                if (pixel_color == 1) {
                    if (row_has_red) { *r = h_sum >> 1; *b = v_sum >> 1; }
                    else             { *b = h_sum >> 1; *r = v_sum >> 1; }
                    *g = val;
                } else if (pixel_color == 0) {
                    *r = val;
                    *g = (h_sum + v_sum) >> 2;
                    *b = (ob_adjust(row_prev[xi-1], subtract_ob, ob_value) + ob_adjust(row_prev[xi+1], subtract_ob, ob_value) +
                          ob_adjust(row_next[xi-1], subtract_ob, ob_value) + ob_adjust(row_next[xi+1], subtract_ob, ob_value)) >> 2;
                } else {
                    *b = val;
                    *g = (h_sum + v_sum) >> 2;
                    *r = (ob_adjust(row_prev[xi-1], subtract_ob, ob_value) + ob_adjust(row_prev[xi+1], subtract_ob, ob_value) +
                          ob_adjust(row_next[xi-1], subtract_ob, ob_value) + ob_adjust(row_next[xi+1], subtract_ob, ob_value)) >> 2;
                }
            } else {
                int r_e=0,g_e=0,b_e=0;
                int val = ob_adjust(row_curr[xi], subtract_ob, ob_value);
                int p = ((int)pattern) & 3;
                int pixel_color = s_bayer_color_lut[p][row_phase][xi & 1];
                int row_has_red = s_row_has_red_lut[p][row_phase];

                int h_sum = 0, h_cnt = 0;
                int v_sum = 0, v_cnt = 0;
                int d_sum = 0, d_cnt = 0;

                if (xi > 0) { h_sum += ob_adjust(row_curr[xi-1], subtract_ob, ob_value); h_cnt++; }
                if (xi < width-1) { h_sum += ob_adjust(row_curr[xi+1], subtract_ob, ob_value); h_cnt++; }
                if (row_prev) { v_sum += ob_adjust(row_prev[xi], subtract_ob, ob_value); v_cnt++; }
                if (row_next) { v_sum += ob_adjust(row_next[xi], subtract_ob, ob_value); v_cnt++; }
                if (row_prev && xi > 0) { d_sum += ob_adjust(row_prev[xi-1], subtract_ob, ob_value); d_cnt++; }
                if (row_prev && xi < width-1) { d_sum += ob_adjust(row_prev[xi+1], subtract_ob, ob_value); d_cnt++; }
                if (row_next && xi > 0) { d_sum += ob_adjust(row_next[xi-1], subtract_ob, ob_value); d_cnt++; }
                if (row_next && xi < width-1) { d_sum += ob_adjust(row_next[xi+1], subtract_ob, ob_value); d_cnt++; }

                if (pixel_color == 1) {
                    g_e = val;
                    if (row_has_red) {
                        if (h_cnt) r_e = h_sum / h_cnt;
                        if (v_cnt) b_e = v_sum / v_cnt;
                    } else {
                        if (h_cnt) b_e = h_sum / h_cnt;
                        if (v_cnt) r_e = v_sum / v_cnt;
                    }
                }
                else if (pixel_color == 0) {
                    r_e = val;
                    if (h_cnt + v_cnt > 0) g_e = (h_sum + v_sum) / (h_cnt + v_cnt);
                    if (d_cnt) b_e = d_sum / d_cnt;
                }
                else {
                    b_e = val;
                    if (h_cnt + v_cnt > 0) g_e = (h_sum + v_sum) / (h_cnt + v_cnt);
                    if (d_cnt) r_e = d_sum / d_cnt;
                }

                *r = r_e; *g = g_e; *b = b_e;
            }
        }

        int r0_i = (r0 * r_gain_fix) >> 8;
        int b0_i = (b0 * b_gain_fix) >> 8;
        int g0_i = (g0 * s_g_gain_fix) >> 8;
        int r1_i = (r1 * r_gain_fix) >> 8;
        int b1_i = (b1 * b_gain_fix) >> 8;
        int g1_i = (g1 * s_g_gain_fix) >> 8;

        r0_i >>= shift_down; g0_i >>= shift_down; b0_i >>= shift_down;
        r1_i >>= shift_down; g1_i >>= shift_down; b1_i >>= shift_down;

        if (r0_i > 255) r0_i = 255; if (g0_i > 255) g0_i = 255; if (b0_i > 255) b0_i = 255;
        if (r1_i > 255) r1_i = 255; if (g1_i > 255) g1_i = 255; if (b1_i > 255) b1_i = 255;

        int rg0 = JPEG_ENC_PACK16(r0_i, g0_i);
        int rg1 = JPEG_ENC_PACK16(r1_i, g1_i);

        int y0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_Y_RG, b0_i * JPEG_ENC_COEF_Y_B) >> 12;
        int y1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_Y_RG, b1_i * JPEG_ENC_COEF_Y_B) >> 12;

        if (y0 < 0) y0 = 0; else if (y0 > 255) y0 = 255;
        if (y1 < 0) y1 = 0; else if (y1 > 255) y1 = 255;
        y0 = s_y_lut[y0];
        y1 = s_y_lut[y1];

        int out_idx = x * 2;
        yuv_out[out_idx + 0] = (uint8_t)y0;
        yuv_out[out_idx + 2] = (uint8_t)y1;
    }
}

// --- Demosaic directly to YUV444 (fast path) ---
__attribute__((hot))
static void demosaic_row_bilinear_to_yuv444_fast(
    const uint16_t* restrict row_prev,
    const uint16_t* restrict row_curr,
    const uint16_t* restrict row_next,
    uint8_t* restrict yuv_out,
    int width,
    int y,
    jpeg_bayer_pattern_t pattern,
    int r_gain_fix,
    int b_gain_fix,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value)
{
    int row_phase = y & 1;

    for (int x = 0; x < width; x += 2)
    {
        int r0=0,g0=0,b0=0;
        int r1=0,g1=0,b1=0;

        for (int i = 0; i < 2; i++)
        {
            int xi = x + i;
            int *r = (i == 0) ? &r0 : &r1;
            int *g = (i == 0) ? &g0 : &g1;
            int *b = (i == 0) ? &b0 : &b1;

            if (xi >= width) {
                *r = r0; *g = g0; *b = b0;
                continue;
            }

            if (row_prev && row_next && xi > 0 && xi < width - 1) {
                int val = ob_adjust(row_curr[xi], subtract_ob, ob_value);
                int p = ((int)pattern) & 3;
                int pixel_color = s_bayer_color_lut[p][row_phase][xi & 1];
                int row_has_red = s_row_has_red_lut[p][row_phase];

                int h_sum = ob_adjust(row_curr[xi-1], subtract_ob, ob_value) + ob_adjust(row_curr[xi+1], subtract_ob, ob_value);
                int v_sum = ob_adjust(row_prev[xi], subtract_ob, ob_value) + ob_adjust(row_next[xi], subtract_ob, ob_value);

                if (pixel_color == 1) {
                    if (row_has_red) { *r = h_sum >> 1; *b = v_sum >> 1; }
                    else             { *b = h_sum >> 1; *r = v_sum >> 1; }
                    *g = val;
                } else if (pixel_color == 0) {
                    *r = val;
                    *g = (h_sum + v_sum) >> 2;
                    *b = (ob_adjust(row_prev[xi-1], subtract_ob, ob_value) + ob_adjust(row_prev[xi+1], subtract_ob, ob_value) +
                          ob_adjust(row_next[xi-1], subtract_ob, ob_value) + ob_adjust(row_next[xi+1], subtract_ob, ob_value)) >> 2;
                } else {
                    *b = val;
                    *g = (h_sum + v_sum) >> 2;
                    *r = (ob_adjust(row_prev[xi-1], subtract_ob, ob_value) + ob_adjust(row_prev[xi+1], subtract_ob, ob_value) +
                          ob_adjust(row_next[xi-1], subtract_ob, ob_value) + ob_adjust(row_next[xi+1], subtract_ob, ob_value)) >> 2;
                }
            } else {
                int r_e=0,g_e=0,b_e=0;
                int val = ob_adjust(row_curr[xi], subtract_ob, ob_value);
                int p = ((int)pattern) & 3;
                int pixel_color = s_bayer_color_lut[p][row_phase][xi & 1];
                int row_has_red = s_row_has_red_lut[p][row_phase];

                int h_sum = 0, h_cnt = 0;
                int v_sum = 0, v_cnt = 0;
                int d_sum = 0, d_cnt = 0;

                if (xi > 0) { h_sum += ob_adjust(row_curr[xi-1], subtract_ob, ob_value); h_cnt++; }
                if (xi < width-1) { h_sum += ob_adjust(row_curr[xi+1], subtract_ob, ob_value); h_cnt++; }
                if (row_prev) { v_sum += ob_adjust(row_prev[xi], subtract_ob, ob_value); v_cnt++; }
                if (row_next) { v_sum += ob_adjust(row_next[xi], subtract_ob, ob_value); v_cnt++; }
                if (row_prev && xi > 0) { d_sum += ob_adjust(row_prev[xi-1], subtract_ob, ob_value); d_cnt++; }
                if (row_prev && xi < width-1) { d_sum += ob_adjust(row_prev[xi+1], subtract_ob, ob_value); d_cnt++; }
                if (row_next && xi > 0) { d_sum += ob_adjust(row_next[xi-1], subtract_ob, ob_value); d_cnt++; }
                if (row_next && xi < width-1) { d_sum += ob_adjust(row_next[xi+1], subtract_ob, ob_value); d_cnt++; }

                if (pixel_color == 1) {
                    g_e = val;
                    if (row_has_red) {
                        if (h_cnt) r_e = h_sum / h_cnt;
                        if (v_cnt) b_e = v_sum / v_cnt;
                    } else {
                        if (h_cnt) b_e = h_sum / h_cnt;
                        if (v_cnt) r_e = v_sum / v_cnt;
                    }
                }
                else if (pixel_color == 0) {
                    r_e = val;
                    if (h_cnt + v_cnt > 0) g_e = (h_sum + v_sum) / (h_cnt + v_cnt);
                    if (d_cnt) b_e = d_sum / d_cnt;
                }
                else {
                    b_e = val;
                    if (h_cnt + v_cnt > 0) g_e = (h_sum + v_sum) / (h_cnt + v_cnt);
                    if (d_cnt) r_e = d_sum / d_cnt;
                }

                *r = r_e; *g = g_e; *b = b_e;
            }
        }

        int r0_i = (r0 * r_gain_fix) >> 8;
        int b0_i = (b0 * b_gain_fix) >> 8;
        int g0_i = (g0 * s_g_gain_fix) >> 8;
        int r1_i = (r1 * r_gain_fix) >> 8;
        int b1_i = (b1 * b_gain_fix) >> 8;
        int g1_i = (g1 * s_g_gain_fix) >> 8;

        r0_i >>= shift_down; g0_i >>= shift_down; b0_i >>= shift_down;
        r1_i >>= shift_down; g1_i >>= shift_down; b1_i >>= shift_down;

        if (r0_i > 255) r0_i = 255; if (g0_i > 255) g0_i = 255; if (b0_i > 255) b0_i = 255;
        if (r1_i > 255) r1_i = 255; if (g1_i > 255) g1_i = 255; if (b1_i > 255) b1_i = 255;

        int rg0 = JPEG_ENC_PACK16(r0_i, g0_i);
        int rg1 = JPEG_ENC_PACK16(r1_i, g1_i);

        int y0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_Y_RG, b0_i * JPEG_ENC_COEF_Y_B) >> 12;
        int cb0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CB_RG, b0_i << 11) >> 12;
        int cr0 = JPEG_ENC_SMLAD(rg0, JPEG_ENC_COEF_CR_RG, b0_i * JPEG_ENC_COEF_CR_B) >> 12;
        int y1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_Y_RG, b1_i * JPEG_ENC_COEF_Y_B) >> 12;
        int cb1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CB_RG, b1_i << 11) >> 12;
        int cr1 = JPEG_ENC_SMLAD(rg1, JPEG_ENC_COEF_CR_RG, b1_i * JPEG_ENC_COEF_CR_B) >> 12;

        cb0 += 128; cr0 += 128; cb1 += 128; cr1 += 128;
        if (y0 < 0) y0 = 0; else if (y0 > 255) y0 = 255;
        if (y1 < 0) y1 = 0; else if (y1 > 255) y1 = 255;
        y0 = s_y_lut[y0];
        y1 = s_y_lut[y1];
        cb0 = clamp_u8(cb0);
        cr0 = clamp_u8(cr0);
        cb1 = clamp_u8(cb1);
        cr1 = clamp_u8(cr1);

        int out_idx = x * 3;
        yuv_out[out_idx + 0] = (uint8_t)y0;
        yuv_out[out_idx + 1] = (uint8_t)cb0;
        yuv_out[out_idx + 2] = (uint8_t)cr0;
        if (x + 1 < width) {
            yuv_out[out_idx + 3] = (uint8_t)y1;
            yuv_out[out_idx + 4] = (uint8_t)cb1;
            yuv_out[out_idx + 5] = (uint8_t)cr1;
        }
    }
}

// Wrapper to select implementation
static void demosaic_row_bilinear(
    const uint16_t* row_prev, 
    const uint16_t* row_curr, 
    const uint16_t* row_next, 
    uint8_t* rgb_out, 
    int width, 
    int y, 
    jpeg_bayer_pattern_t pattern, 
    float r_gain, 
    float b_gain,
    int r_gain_fix,
    int b_gain_fix,
    int shift_down,
    bool subtract_ob,
    uint16_t ob_value,
    bool use_fast) 
{
    bool fast = use_fast;
#if defined(FASTMODE)
    fast = true;
#endif

    if (fast) {
        demosaic_row_bilinear_fast(row_prev, row_curr, row_next, rgb_out, width, y, pattern, r_gain_fix, b_gain_fix, shift_down, subtract_ob, ob_value);
    } else {
        demosaic_row_bilinear_ref(row_prev, row_curr, row_next, rgb_out, width, y, pattern, r_gain, b_gain, shift_down, subtract_ob, ob_value);
    }
}

int jpeg_encode_stream(jpeg_stream_t* stream, const jpeg_encoder_config_t* config) {
    JPEG_TIMING_INIT();
    JPEG_TIMING_FRAME_START();
    
    if (!stream || !stream->read || !stream->write || !config) {
        jpeg_set_error(JPEG_ENCODER_ERR_INVALID_ARGUMENT, "Invalid stream/config arguments", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_INVALID_ARGUMENT;
    }

    int width = config->width;
    int height = config->height;
    if (width <= 0 || height <= 0) {
        jpeg_set_error(JPEG_ENCODER_ERR_INVALID_DIMENSIONS, "Invalid image dimensions", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_INVALID_DIMENSIONS;
    }

    int file_stride = calculate_file_stride(width, config->pixel_format);
    if (file_stride <= 0) {
        jpeg_set_error(JPEG_ENCODER_ERR_INVALID_STRIDE, "Invalid input stride", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_INVALID_STRIDE;
    }

    int downshift = get_downshift_for_format(config->pixel_format);

    init_y_lut();

    init_y_lut();
    
    // Handle Start Offset (Skip Lines)
    if (config->start_offset_lines > 0) {
        // We cannot seek via stream if it doesn't support it, so we read and discard.
        // Even if we have jpeg_seek_callback, that's for the output JPEG stream usually?
        // Wait, stream->read is for input. stream pointer is generic.
        
        // Calculate bytes per line based on INPUT stride, not just pixels.
        // We already have file_stride.
        size_t bytes_to_skip = (size_t)config->start_offset_lines * file_stride;
        
        uint8_t skip_buf[512]; 
        size_t skipped = 0;
        
        while (skipped < bytes_to_skip) {
            size_t ask = sizeof(skip_buf);
            if (ask > bytes_to_skip - skipped) ask = bytes_to_skip - skipped;
            
            size_t r = stream->read(stream->read_ctx, skip_buf, ask);
            if (r == 0) {
                jpeg_set_error(JPEG_ENCODER_ERR_OFFSET_EOF, "EOF while skipping offset", __func__, __LINE__);
                return -(int)JPEG_ENCODER_ERR_OFFSET_EOF;
            }
            skipped += r;
        }
    }

    JPEGE_IMAGE jpege;
    memset(&jpege, 0, sizeof(jpege)); 
    jpege.pfnRead = jpeg_read_callback;
    jpege.pfnWrite = jpeg_write_callback;
    jpege.pfnSeek = jpeg_seek_callback;
    jpege.pfnOpen = jpeg_open_callback;
    jpege.pfnClose = jpeg_close_callback;
    jpege.JPEGFile.fHandle = (void*)stream;
    
    JPEGENCODE je;
    int quality_in = (config->quality > 0) ? config->quality : 85;
    int quality_enum = (quality_in >= 90) ? JPEGE_Q_BEST : ((quality_in >= 75) ? JPEGE_Q_HIGH : ((quality_in >= 50) ? JPEGE_Q_MED : JPEGE_Q_LOW));
    int subsample = JPEGE_SUBSAMPLE_444;
    if (config->subsample == JPEG_SUBSAMPLE_420) {
        subsample = JPEGE_SUBSAMPLE_420;
    } else if (config->subsample == JPEG_SUBSAMPLE_422) {
        subsample = JPEGE_SUBSAMPLE_422;
    }
    
    uint8_t encode_pixel_type = (subsample == JPEGE_SUBSAMPLE_444) ? JPEGE_PIXEL_YUV444 : JPEGE_PIXEL_YUV422;
    if (JPEGEncodeBegin(&jpege, &je, width, height, encode_pixel_type, subsample, quality_enum) != JPEGE_SUCCESS) {
        jpeg_set_error(JPEG_ENCODER_ERR_JPEG_INIT_FAILED, "JPEG encoder initialization failed", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_JPEG_INIT_FAILED;
    }
    
    int mcu_h = (subsample == JPEGE_SUBSAMPLE_420) ? 16 : 8;
    int mcu_w = (subsample == JPEGE_SUBSAMPLE_444) ? 8 : 16;
    // We need indices 1..8 for current block, 0 for prev, 9 for next.
    // So strip[10] lines total.
    
    // Check Memory Usage Limits
    size_t total_alloc = jpeg_encoder_estimate_memory_requirement(config);
    if (total_alloc > JPEG_ENCODER_MAX_MEMORY_USAGE) {
        jpeg_set_error(JPEG_ENCODER_ERR_MEMORY_LIMIT_EXCEEDED, "Memory limit exceeded", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_MEMORY_LIMIT_EXCEEDED;
    }

    int strip_lines = mcu_h + 2; 
    size_t sz_raw = file_stride * strip_lines;
    size_t sz_unpack = width * sizeof(uint16_t) * strip_lines;
    size_t sz_out = (encode_pixel_type == JPEGE_PIXEL_YUV444) ? (width * 3 * mcu_h) : (width * 2 * mcu_h);
    
    if (!jpeg_alloc_reuse((void**)&s_workspace.raw_file_chunk, &s_workspace.raw_size, sz_raw)) {
        jpeg_set_error(JPEG_ENCODER_ERR_ALLOC_RAW_BUFFER, "Failed to allocate raw input buffer", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_ALLOC_RAW_BUFFER;
    }

    if (!jpeg_alloc_reuse((void**)&s_workspace.unpacked_strip, &s_workspace.unpack_size, sz_unpack)) {
        jpeg_set_error(JPEG_ENCODER_ERR_ALLOC_UNPACK_BUFFER, "Failed to allocate unpack buffer", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_ALLOC_UNPACK_BUFFER;
    }
    memset(s_workspace.unpacked_strip, 0, sz_unpack);

    if (!jpeg_alloc_reuse((void**)&s_workspace.out_strip, &s_workspace.out_size, sz_out)) {
        jpeg_set_error(JPEG_ENCODER_ERR_ALLOC_RGB_BUFFER, "Failed to allocate RGB buffer", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_ALLOC_RGB_BUFFER;
    }

    if (!jpeg_alloc_reuse((void**)&s_workspace.carry_over_row, &s_workspace.carry_size, width * sizeof(uint16_t))) {
        jpeg_set_error(JPEG_ENCODER_ERR_ALLOC_CARRY_BUFFER, "Failed to allocate carry-over buffer", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_ALLOC_CARRY_BUFFER;
    }

    if (!jpeg_alloc_reuse((void**)&s_workspace.lookahead_row_save, &s_workspace.lookahead_size, width * sizeof(uint16_t))) {
        jpeg_set_error(JPEG_ENCODER_ERR_ALLOC_LOOKAHEAD_BUFFER, "Failed to allocate lookahead buffer", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_ALLOC_LOOKAHEAD_BUFFER;
    }

    uint8_t* raw_file_chunk = s_workspace.raw_file_chunk;
    uint16_t* unpacked_strip = s_workspace.unpacked_strip;
    uint8_t* out_strip = s_workspace.out_strip;
    uint16_t* carry_over_row = s_workspace.carry_over_row;
    uint16_t* lookahead_row_save = s_workspace.lookahead_row_save;
    
    memset(carry_over_row, 0, width * sizeof(uint16_t)); 
    memset(lookahead_row_save, 0, width * sizeof(uint16_t));
    
    bool use_fast = config->enable_fast_mode;
#if defined(FASTMODE)
    use_fast = true;
#endif

    // Apply calibrated base gains (sensor-specific) for correct WB
    float r_gain = JPEG_DEMOSAIC_RED_GAIN;
    float g_gain = JPEG_DEMOSAIC_GREEN_GAIN;
    float b_gain = JPEG_DEMOSAIC_BLUE_GAIN;
    if (config->apply_awb) {
        if (config->awb_r_gain > 0.0f) r_gain = config->awb_r_gain;
        if (config->awb_g_gain > 0.0f) g_gain = config->awb_g_gain;
        if (config->awb_b_gain > 0.0f) b_gain = config->awb_b_gain;
    }
    s_g_gain = g_gain;
    s_g_gain_fix = (int)(g_gain * 256.0f + 0.5f);
    int r_gain_fix = (int)(r_gain * 256.0f + 0.5f);
    int b_gain_fix = (int)(b_gain * 256.0f + 0.5f);
    
    int total_mcus_y = (height + mcu_h - 1) / mcu_h;
    // int file_lines_read = 0;
    int has_lookahead = 0; // Does strip[1] contain a valid pre-read row?

    for (int mcu_y = 0; mcu_y < total_mcus_y; mcu_y++) {
        int y_start = mcu_y * mcu_h;
        int rows_to_process = mcu_h;
        if (mcu_y == total_mcus_y - 1) rows_to_process = height - y_start;
        
        // Setup strip layout:
        // strip[0] = carry_over (Row y_start-1)
        // strip[1..rows_to_process] = Current Block (Row y_start .. y_start+rows-1)
        // strip[rows_to_process+1] = Lookahead (Row y_start+rows) - Needed for interpolation
        
        // 1. Restore Previous Row
        if (y_start > 0) {
            memcpy(unpacked_strip, carry_over_row, width * sizeof(uint16_t));
        }

        // 2. Prepare Data
        // We need 'rows_to_process' lines + 1 lookahead line.
        // Total needed lines in strip[1...]: count = rows_to_process + (y_start + rows >= h ? 0 : 1)
        
        int lines_needed_in_strip = rows_to_process;
        if (y_start + rows_to_process < height) lines_needed_in_strip++;

        int start_fill_idx = 1;
        if (has_lookahead) {
            // Move Lookahead from previous block (It is in lookahead_row_save)
            memcpy(&unpacked_strip[1 * width], lookahead_row_save, width * sizeof(uint16_t));
            start_fill_idx = 2; // We already have the first line (Row 8, 16, etc.)
        }

        int lines_to_read = lines_needed_in_strip - (has_lookahead ? 1 : 0);
        
        if (lines_to_read > 0) {
            size_t bytes_to_read = lines_to_read * file_stride;
            JPEG_TIMING_START(JPEG_TIMING_RAW_READ);
            size_t br = stream->read(stream->read_ctx, raw_file_chunk, bytes_to_read);
            JPEG_TIMING_END(JPEG_TIMING_RAW_READ);
            
            if (br < bytes_to_read) {
                // Hit EOF or short read: Fill remainder with zeros (Black)
                // This prevents "striping" artifacts from reusing previous buffer data
                memset(raw_file_chunk + br, 0, bytes_to_read - br);
            }
            
            JPEG_TIMING_START(JPEG_TIMING_UNPACK);
            uint8_t* src = raw_file_chunk;
            for (int k = 0; k < lines_to_read; k++) {
                int target_idx = start_fill_idx + k;
                unpack_row(src, &unpacked_strip[target_idx * width], width, config->pixel_format);
                if (config->subtract_ob) {
                    subtract_black_fast(&unpacked_strip[target_idx * width], width, config->ob_value);
                }
                src += file_stride;
            }
            JPEG_TIMING_END(JPEG_TIMING_UNPACK);
            // file_lines_read += lines_to_read;
        }

        // 3. Save Carry Over for NEXT block
        if (rows_to_process > 0) {
            memcpy(carry_over_row, &unpacked_strip[rows_to_process * width], width * sizeof(uint16_t));
        }

        // 4. Mark if we have lookahead for next time
        has_lookahead = (lines_needed_in_strip > rows_to_process);
        if (has_lookahead) {
             memcpy(lookahead_row_save, &unpacked_strip[lines_needed_in_strip * width], width * sizeof(uint16_t));
        }

        // 5. Process rows
        // Hoist loop-invariant decisions and use running pointers to
        // replace per-iteration multiplies with additions.
        JPEG_TIMING_START(JPEG_TIMING_DEMOSAIC);
        
        const int is_yuv444 = (encode_pixel_type == JPEGE_PIXEL_YUV444);
        const int is_420_fast = (!is_yuv444 && use_fast && config->subsample == JPEG_SUBSAMPLE_420);
        const int is_422_fast = (!is_yuv444 && use_fast && !is_420_fast);
        const int out_bpp = is_yuv444 ? 3 : 2;
        const int out_stride = width * out_bpp;
        const jpeg_bayer_pattern_t bayer = config->bayer_pattern;
        const uint16_t ob_val = config->ob_value;
        
        /* Running pointers: avoid i * width multiply per iteration */
        uint16_t* strip_prev = unpacked_strip;                 /* strip[0] */
        uint16_t* strip_curr = unpacked_strip + width;         /* strip[1] */
        uint16_t* strip_next = unpacked_strip + 2 * width;     /* strip[2] */
        uint8_t*  out_row    = out_strip;
        
        for (int i = 0; i < rows_to_process; i++) {
             int abs_y = y_start + i;
             uint16_t* prev = (abs_y > 0)          ? strip_prev : NULL;
             uint16_t* curr = strip_curr;
             uint16_t* next = (abs_y < height - 1) ? strip_next : NULL;
             
             if (is_yuv444) {
                 if (use_fast) {
                     demosaic_row_bilinear_to_yuv444_fast(prev, curr, next, out_row, width, abs_y, bayer, r_gain_fix, b_gain_fix, downshift, false, ob_val);
                 } else {
                     demosaic_row_bilinear_to_yuv444_ref(prev, curr, next, out_row, width, abs_y, bayer, r_gain, b_gain, downshift, false, ob_val);
                 }
             } else if (is_420_fast) {
                 bool can_copy_chroma = ((abs_y & 1) != 0) && (i > 0);
                 if (can_copy_chroma) {
                     demosaic_row_bilinear_to_yuv422_luma_fast(prev, curr, next, out_row, width, abs_y, bayer, r_gain_fix, b_gain_fix, downshift, false, ob_val);
                     /* 32-bit word chroma copy: process 4 bytes per iteration        */
                     /* YUYV layout (little-endian): byte0=Y0, byte1=Cb, byte2=Y1, byte3=Cr */
                     /* Chroma mask 0xFF00FF00 selects Cb and Cr bytes                */
                     uint32_t *dst32 = (uint32_t *)out_row;
                     const uint32_t *prev32 = (const uint32_t *)(out_row - out_stride);
                     const int n_words = width / 2;  /* width pixels / 2 pixels per YUYV group */
                     for (int p = 0; p < n_words; p++) {
                         dst32[p] = (dst32[p] & 0x00FF00FFu) | (prev32[p] & 0xFF00FF00u);
                     }
                 } else {
                     demosaic_row_bilinear_to_yuv422_fast(prev, curr, next, out_row, width, abs_y, bayer, r_gain_fix, b_gain_fix, downshift, false, ob_val);
                 }
             } else if (is_422_fast) {
                 demosaic_row_bilinear_to_yuv422_fast(prev, curr, next, out_row, width, abs_y, bayer, r_gain_fix, b_gain_fix, downshift, false, ob_val);
             } else {
                 demosaic_row_bilinear_to_yuv422_ref(prev, curr, next, out_row, width, abs_y, bayer, r_gain, b_gain, downshift, false, ob_val);
             }
             
             strip_prev += width;
             strip_curr += width;
             strip_next += width;
             out_row    += out_stride;
        }
        JPEG_TIMING_END(JPEG_TIMING_DEMOSAIC);

        JPEG_TIMING_START(JPEG_TIMING_MCU_PREPARE);
        for (int mcu_x = 0; mcu_x < width; mcu_x += mcu_w) {
               int pitch = (encode_pixel_type == JPEGE_PIXEL_YUV444) ? (width * 3) : (width * 2);
               int bpp = (encode_pixel_type == JPEGE_PIXEL_YUV444) ? 3 : 2;
             JPEGAddMCU(&jpege, &je, &out_strip[mcu_x * bpp], pitch);
        }
        JPEG_TIMING_END(JPEG_TIMING_MCU_PREPARE);
    }
    
    JPEGEncodeEnd(&jpege);
    
    return 0;
}

// --- Memory Buffer Stream Wrapper ---

typedef struct {
    const uint8_t* ptr;
    size_t size;
    size_t pos;
} mem_read_ctx_t;

typedef struct {
    uint8_t* ptr;
    size_t capacity;
    size_t pos;
} mem_write_ctx_t;

static size_t mem_read_func(void* ctx, void* buf, size_t size) {
    mem_read_ctx_t* m = (mem_read_ctx_t*)ctx;
    if (m->pos >= m->size) return 0;
    
    size_t avail = m->size - m->pos;
    if (size > avail) size = avail;
    
    memcpy(buf, m->ptr + m->pos, size);
    m->pos += size;
    return size;
}

static size_t mem_write_func(void* ctx, const void* buf, size_t size) {
    mem_write_ctx_t* m = (mem_write_ctx_t*)ctx;
    if (m->pos >= m->capacity) return 0; 
    
    size_t space = m->capacity - m->pos;
    if (size > space) size = space;
    
    memcpy(m->ptr + m->pos, buf, size);
    m->pos += size;
    return size;
}

int jpeg_encode_buffer(const uint8_t* in_buf, size_t in_size, uint8_t* out_buf, size_t out_capacity, size_t* out_size, const jpeg_encoder_config_t* config) {
    if (!in_buf) {
        jpeg_set_error(JPEG_ENCODER_ERR_NULL_IN_BUFFER, "Input buffer is null", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_NULL_IN_BUFFER;
    }
    if (!out_buf) {
        jpeg_set_error(JPEG_ENCODER_ERR_NULL_OUT_BUFFER, "Output buffer is null", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_NULL_OUT_BUFFER;
    }
    if (!out_size) {
        jpeg_set_error(JPEG_ENCODER_ERR_NULL_OUT_SIZE, "Output size pointer is null", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_NULL_OUT_SIZE;
    }
    if (!config) {
        jpeg_set_error(JPEG_ENCODER_ERR_INVALID_ARGUMENT, "Config is null", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_INVALID_ARGUMENT;
    }
    if (out_capacity == 0) {
        jpeg_set_error(JPEG_ENCODER_ERR_ZERO_OUT_CAPACITY, "Output buffer capacity is zero", __func__, __LINE__);
        return -(int)JPEG_ENCODER_ERR_ZERO_OUT_CAPACITY;
    }

    mem_read_ctx_t ctx_in = { .ptr = in_buf, .size = in_size, .pos = 0 };
    mem_write_ctx_t ctx_out = { .ptr = out_buf, .capacity = out_capacity, .pos = 0 };

    jpeg_stream_t stream;
    stream.read = mem_read_func;
    stream.read_ctx = &ctx_in;
    stream.write = mem_write_func;
    stream.write_ctx = &ctx_out;

    int res = jpeg_encode_stream(&stream, config);

    if (res == 0) {
        *out_size = ctx_out.pos;
    }
    return res;
}

