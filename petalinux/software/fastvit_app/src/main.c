/*================================================================
 * main.c — FastVIT-T8 Linux 推理应用 (Petalinux)
 *
 * 与裸机版的主要差异:
 *   - AXI-Lite: /dev/mem + mmap (而非直接寄存器访问)
 *   - 权重加载: Linux fopen/fread (而非 FatFs)
 *   - 计时: clock_gettime (而非 XTime)
 *   - 打印: printf (而非 xil_printf)
 *   - DMA 缓冲区: 物理保留内存 (通过 /dev/mem 映射)
 *
 * 内存布局 (需在 Petalinux device tree 中保留):
 *   0x10000000: 权重 (~3.2 MB)
 *   0x12100000: Ping buffer (2 MB)
 *   0x12300000: Pong buffer (2 MB)
 *   0x12500000: Temp buffer (1 MB)
 *
 * 编译: 参考 Makefile
 * 运行: sudo ./fastvit_infer [weights_dir]
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

/* ── DMA 物理内存基地址 ──────────────────────────────────── */
#define FV_DDR_BASE       0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define FV_FEAT_TEMP_BASE 0x12500000UL

/* 总 DMA 区域大小 (覆盖权重+缓冲区) */
#define FV_DMA_SIZE       0x06000000UL  /* 96 MB */

/* ── 计时工具 ─────────────────────────────────────────────── */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/* ── DMA 缓冲区映射 ───────────────────────────────────────── */
static int  fd_dma = -1;
static void *dma_base_virt = NULL;

static int dma_init(void) {
    fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem for DMA"); return -1; }

    dma_base_virt = mmap(NULL, FV_DMA_SIZE,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd_dma, FV_DDR_BASE);
    if (dma_base_virt == MAP_FAILED) {
        perror("mmap DMA region");
        return -1;
    }
    printf("[DMA] Mapped 0x%08lX - 0x%08lX → virt %p\n",
           FV_DDR_BASE, FV_DDR_BASE + FV_DMA_SIZE, dma_base_virt);
    return 0;
}

static void dma_exit(void) {
    if (dma_base_virt) munmap(dma_base_virt, FV_DMA_SIZE);
    if (fd_dma >= 0)   close(fd_dma);
}

/* 物理地址 → 虚拟地址 (在 DMA 映射窗口内) */
static void* phys_to_virt(uintptr_t phys) {
    return (char*)dma_base_virt + (phys - FV_DDR_BASE);
}

/* ── 权重加载 ─────────────────────────────────────────────── */
static int load_weights(const char *weights_dir) {
    uint8_t *ddr = (uint8_t*)phys_to_virt(FV_DDR_BASE);
    uint32_t total = 0;

    printf("[Weights] Loading from: %s\n", weights_dir);

    for (int i = 0; i < FV_NUM_LAYERS; i++) {
        /* weight */
        if (FV_WEIGHT_SIZES[i] > 0 && FV_WEIGHT_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_WEIGHT_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            size_t r = fread(ddr + FV_W_OFFSETS[i], 1, FV_WEIGHT_SIZES[i], f);
            fclose(f);
            total += (uint32_t)r;
        }
        /* bias */
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

    /* 确保数据写入物理内存 (sync) */
    msync(dma_base_virt, FV_WEIGHT_TOTAL_BYTES, MS_SYNC);

    printf("[Weights] Loaded %u bytes (%u KB)\n", total, total / 1024);
    return 0;
}

/* ─────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    const char *weights_dir = (argc > 1) ? argv[1] : "./weights_t8";

    printf("\n");
    printf("=========================================\n");
    printf("  FastVIT-T8 FPGA Inference (Linux)\n");
    printf("  Target: MicroZed xc7z020 @ 200MHz\n");
    printf("=========================================\n");

    /* 1. 初始化 IP 驱动 (映射 AXI-Lite) */
    if (fv_driver_init() != 0) {
        fprintf(stderr, "FATAL: driver init failed\n");
        return 1;
    }

    /* 2. 映射 DMA 内存区域 */
    if (dma_init() != 0) {
        fprintf(stderr, "FATAL: DMA init failed\n");
        fv_driver_exit();
        return 1;
    }

    /* 3. 加载权重 */
    if (load_weights(weights_dir) != 0) {
        fprintf(stderr, "FATAL: weight load failed\n");
        goto cleanup;
    }

    /* 4. 建立 LayerWeight 映射 (使用物理地址供 IP 访问) */
    static LayerWeight lw[FV_NUM_LAYERS];
    fv_build_layer_weights(lw, FV_DDR_BASE);
    printf("[Init] LayerWeight built for %d layers\n", FV_NUM_LAYERS);

    /* 5. 准备缓冲区虚拟地址 (CPU 访问) */
    int8_t *ping_virt = (int8_t*)phys_to_virt(FV_FEAT_PING_BASE);
    int8_t *pong_virt = (int8_t*)phys_to_virt(FV_FEAT_PONG_BASE);

    /* 6. 准备测试输入 (全零) */
    static int8_t test_input[3 * 128 * 128];
    static int8_t test_output[768 * 4 * 4];
    memset(test_input, 0, sizeof(test_input));

    /* 7. 热身 */
    printf("[Warmup] ...\n");
    fastvit_t8_infer(test_input, test_output, lw, ping_virt, pong_virt);

    /* 8. 计时推理 */
    printf("[Infer] Starting...\n");
    double t0 = get_time_ms();
    int ret = fastvit_t8_infer(test_input, test_output, lw, ping_virt, pong_virt);
    double t1 = get_time_ms();

    printf("[Infer] Return: %d\n", ret);
    printf("[Timing] %.1f ms\n", t1 - t0);
    printf("[Output] output[0..7]:");
    for (int i = 0; i < 8; i++) printf(" %4d", (int)test_output[i]);
    printf("\n");

    int nonzero = 0;
    for (int i = 0; i < 768 * 4 * 4; i++)
        if (test_output[i] != 0) nonzero++;
    printf("[Check] Non-zero: %d / %d\n", nonzero, 768 * 4 * 4);

    printf("=========================================\n");
    printf("  DONE\n");
    printf("=========================================\n");

cleanup:
    dma_exit();
    fv_driver_exit();
    return 0;
}
