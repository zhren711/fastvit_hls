/*================================================================
 * accuracy_infer.c — Phase 0.5 board-side accuracy harness.
 *
 * Runs the UNMODIFIED, as-deployed fastvit_infer.c (same source the
 * production fastvit_infer_v18gelu binary links against -- this
 * program links it in directly, not a debug variant) against a
 * real image file instead of the driver's built-in all-zero test
 * input, and writes the raw int8 [768,4,4] output to a file for
 * comparison against the ONNX float32 reference (see
 * tools/run_accuracy_harness.py / compare_accuracy_results.py).
 *
 * 用法: ./accuracy_infer <input.bin> <output.bin> [weights_dir]
 *   input.bin:  raw int8 [3,128,128] (from run_accuracy_harness.py)
 *   output.bin: raw int8 [768,4,4] written here
 *================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "weights_layout.h"

#define FV_DDR_BASE       0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define FV_DMA_SIZE       0x06000000UL

static int fd_dma = -1;
static void *dma_base_virt = NULL;

static int dma_init(void) {
    fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem"); return -1; }
    dma_base_virt = mmap(NULL, FV_DMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_base_virt == MAP_FAILED) { perror("mmap DMA"); return -1; }
    return 0;
}

static void *phys_to_virt(uintptr_t phys) {
    return (char *)dma_base_virt + (phys - FV_DDR_BASE);
}

static int load_weights(const char *weights_dir) {
    uint8_t *ddr = (uint8_t *)phys_to_virt(FV_DDR_BASE);
    for (int i = 0; i < FV_NUM_LAYERS; i++) {
        if (FV_WEIGHT_SIZES[i] > 0 && FV_WEIGHT_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_WEIGHT_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            fread(ddr + FV_W_OFFSETS[i], 1, FV_WEIGHT_SIZES[i], f);
            fclose(f);
        }
        if (FV_BIAS_SIZES[i] > 0 && FV_BIAS_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_BIAS_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            fread(ddr + FV_B_OFFSETS[i], 1, FV_BIAS_SIZES[i], f);
            fclose(f);
        }
    }
    msync(dma_base_virt, FV_WEIGHT_TOTAL_BYTES, MS_SYNC);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.bin> <output.bin> [weights_dir]\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    const char *weights_dir = (argc > 3) ? argv[3] : "/home/root/weights_t8";

    if (fv_driver_init() != 0) return 1;
    if (dma_init() != 0) { fv_driver_exit(); return 1; }
    if (load_weights(weights_dir) != 0) { fv_driver_exit(); return 1; }

    static LayerWeight lw[FV_NUM_LAYERS];
    fv_build_layer_weights(lw, FV_DDR_BASE);

    int8_t *ping_virt = (int8_t *)phys_to_virt(FV_FEAT_PING_BASE);
    int8_t *pong_virt = (int8_t *)phys_to_virt(FV_FEAT_PONG_BASE);

    static int8_t test_input[3 * 128 * 128];
    static int8_t test_output[768 * 4 * 4];

    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror("open input"); return 1; }
    size_t rd = fread(test_input, 1, sizeof(test_input), fin);
    fclose(fin);
    if (rd != sizeof(test_input)) {
        fprintf(stderr, "WARNING: read %zu bytes, expected %zu\n", rd, sizeof(test_input));
    }

    int ret = fastvit_t8_infer(test_input, test_output, lw, ping_virt, pong_virt);

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror("open output"); return 1; }
    fwrite(test_output, 1, sizeof(test_output), fout);
    fclose(fout);

    int nonzero = 0;
    for (int i = 0; i < 768 * 4 * 4; i++) if (test_output[i] != 0) nonzero++;
    fprintf(stderr, "[accuracy_infer] %s -> %s  ret=%d  nonzero=%d/%d\n",
            in_path, out_path, ret, nonzero, 768 * 4 * 4);

    fv_driver_exit();
    return 0;
}
