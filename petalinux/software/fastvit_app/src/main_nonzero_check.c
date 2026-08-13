/*================================================================
 * main_nonzero_check.c — Phase 0.6 decisive test (ZHR-8's
 * "SE block all-zero output" bug, Non-zero: 0/12288).
 *
 * Hypothesis: the existing smoke test (main.c) feeds an ALL-ZERO
 * test image and treats "Non-zero: 0/12288" as a failure signal.
 * With 52 sequential int8-quantized layers each doing
 * (acc >> out_shift) with saturation, a genuinely all-black image
 * can legitimately round bias-only activations to 0 at an early
 * layer and then propagate 0 all the way through -- with NO bug
 * anywhere, on ANY input that happens to be exactly zero. This is
 * a separate question from whether the bitstream is numerically
 * correct on real (non-degenerate) inputs.
 *
 * This program is a near-verbatim copy of main.c (same driver
 * facade, same fastvit_t8_infer() call, unmodified) with ONE
 * change: test_input is a deterministic non-zero synthetic pattern
 * instead of memset(0). Builds to a SEPARATE binary
 * (fastvit_nonzero_check) -- does not touch or overwrite the
 * production fastvit_infer binary or the v18gelu bitstream.
 *================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "weights_layout.h"

#define FV_DDR_BASE       0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define FV_FEAT_TEMP_BASE 0x12500000UL
#define FV_DMA_SIZE       0x06000000UL

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int  fd_dma = -1;
static void *dma_base_virt = NULL;

static int dma_init(void) {
    fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem for DMA"); return -1; }
    dma_base_virt = mmap(NULL, FV_DMA_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_base_virt == MAP_FAILED) { perror("mmap DMA region"); return -1; }
    printf("[DMA] Mapped 0x%08lX - 0x%08lX -> virt %p\n",
           FV_DDR_BASE, FV_DDR_BASE + FV_DMA_SIZE, dma_base_virt);
    return 0;
}

static void dma_exit(void) {
    if (dma_base_virt) munmap(dma_base_virt, FV_DMA_SIZE);
    if (fd_dma >= 0)   close(fd_dma);
}

static void* phys_to_virt(uintptr_t phys) {
    return (char*)dma_base_virt + (phys - FV_DDR_BASE);
}

static int load_weights(const char *weights_dir) {
    uint8_t *ddr = (uint8_t*)phys_to_virt(FV_DDR_BASE);
    uint32_t total = 0;
    printf("[Weights] Loading from: %s\n", weights_dir);
    for (int i = 0; i < FV_NUM_LAYERS; i++) {
        if (FV_WEIGHT_SIZES[i] > 0 && FV_WEIGHT_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_WEIGHT_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            size_t r = fread(ddr + FV_W_OFFSETS[i], 1, FV_WEIGHT_SIZES[i], f);
            fclose(f);
            total += (uint32_t)r;
        }
        if (FV_BIAS_SIZES[i] > 0 && FV_BIAS_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_BIAS_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            size_t r = fread(ddr + FV_B_OFFSETS[i], 1, FV_BIAS_SIZES[i], f);
            fclose(f);
            total += (uint32_t)r;
        }
    }
    msync(dma_base_virt, FV_WEIGHT_TOTAL_BYTES, MS_SYNC);
    printf("[Weights] Loaded %u bytes (%u KB)\n", total, total / 1024);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *weights_dir = (argc > 1) ? argv[1] : "./weights_t8";

    printf("\n");
    printf("=========================================\n");
    printf("  FastVIT-T8 Phase-0.6 nonzero-input check\n");
    printf("  (same driver/infer code as production;\n");
    printf("   only the test image differs from main.c)\n");
    printf("=========================================\n");

    if (fv_driver_init() != 0) { fprintf(stderr, "FATAL: driver init failed\n"); return 1; }
    if (dma_init() != 0) { fv_driver_exit(); return 1; }
    if (load_weights(weights_dir) != 0) { goto cleanup; }

    static LayerWeight lw[FV_NUM_LAYERS];
    fv_build_layer_weights(lw, FV_DDR_BASE);
    printf("[Init] LayerWeight built for %d layers\n", FV_NUM_LAYERS);

    int8_t *ping_virt = (int8_t*)phys_to_virt(FV_FEAT_PING_BASE);
    int8_t *pong_virt = (int8_t*)phys_to_virt(FV_FEAT_PONG_BASE);

    static int8_t test_input[3 * 128 * 128];
    static int8_t test_output[768 * 4 * 4];
    /* Deterministic non-degenerate pattern spanning most of the int8
     * range -- NOT all-zero, NOT a single repeated constant (a single
     * constant could still degenerate under some pathological weight
     * pattern; this at least varies per-pixel/per-channel). */
    for (int i = 0; i < 3 * 128 * 128; i++)
        test_input[i] = (int8_t)(((i * 37 + 13) % 251) - 125);

    printf("[Warmup] ...\n");
    fastvit_t8_infer(test_input, test_output, lw, ping_virt, pong_virt);

    printf("[Infer] Starting...\n");
    double t0 = get_time_ms();
    int ret = fastvit_t8_infer(test_input, test_output, lw, ping_virt, pong_virt);
    double t1 = get_time_ms();

    printf("[Infer] Return: %d\n", ret);
    printf("[Timing] %.1f ms\n", t1 - t0);
    printf("[Input]  input[0..7]:");
    for (int i = 0; i < 8; i++) printf(" %4d", (int)test_input[i]);
    printf("\n");
    printf("[Output] output[0..7]:");
    for (int i = 0; i < 8; i++) printf(" %4d", (int)test_output[i]);
    printf("\n");

    int nonzero = 0;
    int32_t sum_abs = 0;
    int8_t vmin = 127, vmax = -128;
    for (int i = 0; i < 768 * 4 * 4; i++) {
        int8_t v = test_output[i];
        if (v != 0) nonzero++;
        sum_abs += (v < 0 ? -v : v);
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    printf("[Check] Non-zero: %d / %d\n", nonzero, 768 * 4 * 4);
    printf("[Check] min=%d max=%d mean_abs=%.3f\n",
           (int)vmin, (int)vmax, (double)sum_abs / (768.0 * 4 * 4));

    printf("=========================================\n");
    printf("  DONE\n");
    printf("=========================================\n");

cleanup:
    dma_exit();
    fv_driver_exit();
    return 0;
}
