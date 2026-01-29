#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include "jpeg_encoder.h"

// Input Parameters
#define INPUT_FILENAME "frame_20260114.bin"
#define OUTPUT_FILENAME_FAST_420 "output_fast_420.jpg"
#define OUTPUT_FILENAME_SLOW_420 "output_slow_420.jpg"
#define OUTPUT_FILENAME_BUFFER_FAST_420 "output_buffer_fast_420.jpg"
#define OUTPUT_FILENAME_BUFFER_SLOW_420 "output_buffer_slow_420.jpg"
#define OUTPUT_FILENAME_FAST_422 "output_fast_422.jpg"
#define OUTPUT_FILENAME_SLOW_422 "output_slow_422.jpg"
#define OUTPUT_FILENAME_BUFFER_FAST_422 "output_buffer_fast_422.jpg"
#define OUTPUT_FILENAME_BUFFER_SLOW_422 "output_buffer_slow_422.jpg"
#define OUTPUT_FILENAME_FAST_444 "output_fast_444.jpg"
#define OUTPUT_FILENAME_SLOW_444 "output_slow_444.jpg"
#define OUTPUT_FILENAME_BUFFER_FAST_444 "output_buffer_fast_444.jpg"
#define OUTPUT_FILENAME_BUFFER_SLOW_444 "output_buffer_slow_444.jpg"

#define IMG_WIDTH 640
#define IMG_HEIGHT 400
#define PIXEL_FORMAT JPEG_PIXEL_FORMAT_BAYER12_GRGB // Unpacked 16-bit GRGB
#define BAYER_PATTERN JPEG_BAYER_PATTERN_GBRG
#define AWB_RED_GAIN 1.375000f
#define AWB_GREEN_GAIN 0.970000f
#define AWB_BLUE_GAIN 1.200000f

static float g_awb_r_gain = AWB_RED_GAIN;
static float g_awb_g_gain = AWB_GREEN_GAIN;
static float g_awb_b_gain = AWB_BLUE_GAIN;

// --- Stream Interface ---

typedef struct {
    FILE* fp;
} file_ctx_t;

size_t file_read(void* ctx, void* buf, size_t size) {
    file_ctx_t* f = (file_ctx_t*)ctx;
    return fread(buf, 1, size, f->fp);
}

size_t file_write(void* ctx, const void* buf, size_t size) {
    file_ctx_t* f = (file_ctx_t*)ctx;
    return fwrite(buf, 1, size, f->fp);
}

// --- Utils ---

double get_wall_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void print_last_error(const char* context) {
    jpeg_encoder_error_t err;
    if (jpeg_encoder_get_last_error(&err) == 0) {
        fprintf(stderr, "%s: code=%d, msg=%s, at %s:%d\n",
                context,
                (int)err.code,
                err.message ? err.message : "(null)",
                err.function ? err.function : "(unknown)",
                err.line);
    }
}

// --- Benchmark Runner ---

size_t run_benchmark_pass(const char* mode_name, bool fast_mode, jpeg_subsample_t subsample, const char* out_filename, size_t raw_size) {
    printf("\n=== Running [%s] Pass ===\n", mode_name);
    
    FILE* fin = fopen(INPUT_FILENAME, "rb");
    if (!fin) {
        fprintf(stderr, "Error: Could not open input file %s\n", INPUT_FILENAME);
        return 0;
    }

    FILE* fout = fopen(out_filename, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Could not open output file %s\n", out_filename);
        fclose(fin);
        return 0;
    }

    file_ctx_t ctx_in = { .fp = fin };
    file_ctx_t ctx_out = { .fp = fout };

    jpeg_stream_t stream;
    stream.read = file_read;
    stream.read_ctx = &ctx_in;
    stream.write = file_write;
    stream.write_ctx = &ctx_out;

    jpeg_encoder_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = IMG_WIDTH;
    config.height = IMG_HEIGHT;
    config.pixel_format = PIXEL_FORMAT;
    config.bayer_pattern = BAYER_PATTERN;
    config.start_offset_lines = 2; // Setup Offset Here
    config.quality = 90;
    config.ob_value = 0; 
    config.subtract_ob = false;
    config.apply_awb = true;
    config.awb_r_gain = g_awb_r_gain;
    config.awb_g_gain = g_awb_g_gain;
    config.awb_b_gain = g_awb_b_gain;
    config.enable_fast_mode = fast_mode; // Control Flag
    config.subsample = subsample;

    // Warmup / Cache Priming? Maybe not needed for simple FS tests.
    
    clock_t cpu_start = clock();
    double wall_start = get_wall_time_ms();

    int res = jpeg_encode_stream(&stream, &config);
    
    double wall_end = get_wall_time_ms();
    clock_t cpu_end = clock();

    size_t out_size = 0;
    if (res == 0) {
        double cpu_ms = ((double)(cpu_end - cpu_start) / CLOCKS_PER_SEC) * 1000.0;
        double wall_ms = wall_end - wall_start;
        out_size = (size_t)ftell(fout);
        
        printf("Result: SUCCESS\n");
        printf("Time (Wall): %.3f ms\n", wall_ms);
        printf("Time (CPU):  %.3f ms\n", cpu_ms);
        printf("Output Size: %zu bytes\n", out_size);
        if (raw_size > 0 && out_size > 0) {
            printf("Compression Ratio: %.2fx\n", (double)raw_size / (double)out_size);
        }
    } else {
        printf("Result: FAILED (%d)\n", res);
        print_last_error("stream encode");
    }

    fclose(fin);
    fclose(fout);
    return out_size;
}

size_t run_buffer_benchmark_pass(const char* mode_name, bool fast_mode, jpeg_subsample_t subsample, const char* out_filename, size_t raw_size) {
    printf("\n=== Running [Buffer Mode: %s] Pass ===\n", mode_name);
    
    FILE* fin = fopen(INPUT_FILENAME, "rb");
    if (!fin) {
        fprintf(stderr, "Error: Could not open input file %s\n", INPUT_FILENAME);
        return 0;
    }
    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    
    uint8_t* in_buf = (uint8_t*)malloc(fsize);
    if (!in_buf) {
        fprintf(stderr, "Error: Could not allocate input buffer (%ld bytes)\n", fsize);
        fclose(fin);
        return 0;
    }
    if (fread(in_buf, 1, fsize, fin) != fsize) {
        fprintf(stderr, "Error: Could not read entire file\n");
        free(in_buf);
        fclose(fin);
        return 0;
    }
    fclose(fin);

    size_t out_capacity = IMG_WIDTH * IMG_HEIGHT * 3; 
    uint8_t* out_buf = (uint8_t*)malloc(out_capacity);
    if (!out_buf) {
        fprintf(stderr, "Error: Could not allocate output buffer\n");
        free(in_buf);
        return 0;
    }

    jpeg_encoder_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = IMG_WIDTH;
    config.height = IMG_HEIGHT;
    config.pixel_format = PIXEL_FORMAT;
    config.bayer_pattern = BAYER_PATTERN;
    config.start_offset_lines = 2; 
    config.quality = 90;
    config.ob_value = 0; 
    config.subtract_ob = false;
    config.apply_awb = true;
    config.awb_r_gain = g_awb_r_gain;
    config.awb_g_gain = g_awb_g_gain;
    config.awb_b_gain = g_awb_b_gain;
    config.enable_fast_mode = fast_mode; 
    config.subsample = subsample;

    size_t out_size = 0;
    
    clock_t cpu_start = clock();
    double wall_start = get_wall_time_ms();

    int res = jpeg_encode_buffer(in_buf, fsize, out_buf, out_capacity, &out_size, &config);
    
    double wall_end = get_wall_time_ms();
    clock_t cpu_end = clock();

    if (res == 0) {
        double cpu_ms = ((double)(cpu_end - cpu_start) / CLOCKS_PER_SEC) * 1000.0;
        double wall_ms = wall_end - wall_start;
        
        printf("Result: SUCCESS\n");
        printf("Time (Wall): %.3f ms\n", wall_ms);
        printf("Time (CPU):  %.3f ms\n", cpu_ms);
        printf("Output Size: %zu bytes\n", out_size);
        if (raw_size > 0 && out_size > 0) {
            printf("Compression Ratio: %.2fx\n", (double)raw_size / (double)out_size);
        }
        
        FILE* fout = fopen(out_filename, "wb");
        if (fout) {
            fwrite(out_buf, 1, out_size, fout);
            fclose(fout);
        } else {
            fprintf(stderr, "Error: Could not write output file\n");
        }
    } else {
        printf("Result: FAILED (%d)\n", res);
        print_last_error("buffer encode");
    }

    free(in_buf);
    free(out_buf);
    return out_size;
}


int main(int argc, char** argv) {
    printf("JPEG Encoder Comparison Test\n");
    printf("Input: %s (%dx%d 16-bit Bayer)\n", INPUT_FILENAME, IMG_WIDTH, IMG_HEIGHT);
    size_t raw_size = 0;
    {
        FILE* fin = fopen(INPUT_FILENAME, "rb");
        if (fin) {
            fseek(fin, 0, SEEK_END);
            raw_size = (size_t)ftell(fin);
            fclose(fin);
        }
    }

    const char* r_env = getenv("AWB_R_GAIN");
    const char* g_env = getenv("AWB_G_GAIN");
    const char* b_env = getenv("AWB_B_GAIN");
    if (r_env && *r_env) {
        g_awb_r_gain = strtof(r_env, NULL);
    }
    if (g_env && *g_env) {
        g_awb_g_gain = strtof(g_env, NULL);
    }
    if (b_env && *b_env) {
        g_awb_b_gain = strtof(b_env, NULL);
    }

    const char* only_fast_444 = getenv("JPEG_TEST_ONLY_FAST_444");
    if (only_fast_444 && strcmp(only_fast_444, "1") == 0) {
        run_benchmark_pass("Fast Mode (Q8 Fixed) 4:4:4", true, JPEG_SUBSAMPLE_444, OUTPUT_FILENAME_FAST_444, raw_size);
        printf("\nDone.\n");
        return 0;
    }
    
    // 1. Reference (Slow) Mode - 4:2:0
    run_benchmark_pass("Reference (Float/Slow) 4:2:0", false, JPEG_SUBSAMPLE_420, OUTPUT_FILENAME_SLOW_420, raw_size);
    
    // 2. Fast Mode - 4:2:0
    run_benchmark_pass("Fast Mode (Q8 Fixed) 4:2:0", true, JPEG_SUBSAMPLE_420, OUTPUT_FILENAME_FAST_420, raw_size);

    // 3. Reference (Buffer) Mode - 4:2:0
    run_buffer_benchmark_pass("Reference (Float/Slow) 4:2:0", false, JPEG_SUBSAMPLE_420, OUTPUT_FILENAME_BUFFER_SLOW_420, raw_size);
    
    // 4. Fast (Buffer) Mode - 4:2:0
    run_buffer_benchmark_pass("Fast Mode (Q8 Fixed) 4:2:0", true, JPEG_SUBSAMPLE_420, OUTPUT_FILENAME_BUFFER_FAST_420, raw_size);

    // 5. Reference (Slow) Mode - 4:2:2
    run_benchmark_pass("Reference (Float/Slow) 4:2:2", false, JPEG_SUBSAMPLE_422, OUTPUT_FILENAME_SLOW_422, raw_size);
    
    // 6. Fast Mode - 4:2:2
    run_benchmark_pass("Fast Mode (Q8 Fixed) 4:2:2", true, JPEG_SUBSAMPLE_422, OUTPUT_FILENAME_FAST_422, raw_size);

    // 7. Reference (Buffer) Mode - 4:2:2
    run_buffer_benchmark_pass("Reference (Float/Slow) 4:2:2", false, JPEG_SUBSAMPLE_422, OUTPUT_FILENAME_BUFFER_SLOW_422, raw_size);
    
    // 8. Fast (Buffer) Mode - 4:2:2
    run_buffer_benchmark_pass("Fast Mode (Q8 Fixed) 4:2:2", true, JPEG_SUBSAMPLE_422, OUTPUT_FILENAME_BUFFER_FAST_422, raw_size);

    // 9. Reference (Slow) Mode - 4:4:4
    run_benchmark_pass("Reference (Float/Slow) 4:4:4", false, JPEG_SUBSAMPLE_444, OUTPUT_FILENAME_SLOW_444, raw_size);

    // 10. Fast Mode - 4:4:4
    run_benchmark_pass("Fast Mode (Q8 Fixed) 4:4:4", true, JPEG_SUBSAMPLE_444, OUTPUT_FILENAME_FAST_444, raw_size);

    // 11. Reference (Buffer) Mode - 4:4:4
    run_buffer_benchmark_pass("Reference (Float/Slow) 4:4:4", false, JPEG_SUBSAMPLE_444, OUTPUT_FILENAME_BUFFER_SLOW_444, raw_size);

    // 12. Fast (Buffer) Mode - 4:4:4
    run_buffer_benchmark_pass("Fast Mode (Q8 Fixed) 4:4:4", true, JPEG_SUBSAMPLE_444, OUTPUT_FILENAME_BUFFER_FAST_444, raw_size);

    printf("\nDone.\n");
    return 0;
}
