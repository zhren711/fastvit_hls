/*=============================================================
 * main.c  —  FastVIT-T8 FPGA 推理 Vitis SDK 裸机应用
 *
 * 功能:
 *   1. 从 SD 卡加载 52 层 int8 权重 → DDR (FV_DDR_BASE)
 *   2. 调用 fastvit_t8_infer() 跑一次推理
 *   3. 用 XTime 计时，通过 UART 输出结果
 *
 * 硬件依赖:
 *   - xparameters.h (Vivado BSP 自动生成)
 *   - Zynq PS7 DDR: 0x0000_0000 ~ 0x1FFF_FFFF (512MB)
 *   - SD 卡: FAT32 格式，根目录存放 weights_t8/ 文件夹
 *
 * 编译说明 (Vitis SDK):
 *   - 新建 Standalone BSP，添加 FatFs 库 (xilffs)
 *   - 添加本目录所有 .c / .h 文件
 *   - Linker script: 确保 .text / .data 不超过 4MB BRAM/DDR
 *
 * 内存布局:
 *   0x0000_0000: ELF code + data (< 8 MB)
 *   0x1000_0000: 权重 (FV_DDR_BASE, ~3.2 MB)
 *   0x1210_0000: Ping buffer (2 MB)
 *   0x1230_0000: Pong buffer (2 MB)
 *   0x1250_0000: Temp buffer (1 MB)
 *=============================================================*/

#include "xparameters.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xtime_l.h"

/* FatFs (xilffs library) */
#include "ff.h"

#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "weights_layout.h"   /* 由 gen_weights_layout.py 生成 */

/* ── 测试输入图像 (全零 = 纯黑图) ────────────────────────── */
static int8_t  test_input[3 * 128 * 128];   /* int8 NCHW */
static int8_t  test_output[768 * 4 * 4];

/* ── LayerWeight 数组 ─────────────────────────────────────── */
static LayerWeight lw[FV_NUM_LAYERS];

/* ── FatFs 对象 ─────────────────────────────────────────────  */
static FATFS fatfs;

/* ─────────────────────────────────────────────────────────── */
static int load_weights_from_sd(void)
{
    FIL   fil;
    UINT  br;
    FRESULT res;
    uint8_t *ddr = (uint8_t *)FV_DDR_BASE;

    xil_printf("[FatFs] Mounting SD card...\r\n");
    res = f_mount(&fatfs, "0:/", 1);
    if (res != FR_OK) {
        xil_printf("ERROR: f_mount failed: %d\r\n", res);
        return -1;
    }

    uint32_t total_loaded = 0;

    for (int i = 0; i < FV_NUM_LAYERS; i++) {
        /* ── load weight ── */
        if (FV_WEIGHT_SIZES[i] > 0 && FV_WEIGHT_FILES[i][0] != '\0') {
            char path[128];
            snprintf(path, sizeof(path), "0:/weights_t8/%s", FV_WEIGHT_FILES[i]);
            res = f_open(&fil, path, FA_READ);
            if (res != FR_OK) {
                xil_printf("ERROR: open %s failed: %d\r\n", path, res);
                return -1;
            }
            f_read(&fil, ddr + FV_W_OFFSETS[i], FV_WEIGHT_SIZES[i], &br);
            f_close(&fil);
            total_loaded += br;
        }

        /* ── load bias ── */
        if (FV_BIAS_SIZES[i] > 0 && FV_BIAS_FILES[i][0] != '\0') {
            char path[128];
            snprintf(path, sizeof(path), "0:/weights_t8/%s", FV_BIAS_FILES[i]);
            res = f_open(&fil, path, FA_READ);
            if (res != FR_OK) {
                xil_printf("ERROR: open %s failed: %d\r\n", path, res);
                return -1;
            }
            f_read(&fil, ddr + FV_B_OFFSETS[i], FV_BIAS_SIZES[i], &br);
            f_close(&fil);
            total_loaded += br;
        }
    }

    xil_printf("[FatFs] Loaded %u bytes (%u KB) from SD\r\n",
               total_loaded, total_loaded / 1024);

    /* 刷新 D-cache: 确保 FPGA DMA 能看到最新数据 */
    Xil_DCacheFlushRange(FV_DDR_BASE, FV_WEIGHT_TOTAL_BYTES);
    return 0;
}

/* ─────────────────────────────────────────────────────────── */
int main(void)
{
    xil_printf("\r\n");
    xil_printf("=========================================\r\n");
    xil_printf("  FastVIT-T8 FPGA Inference @ 200 MHz\r\n");
    xil_printf("=========================================\r\n");

    /* 1. 加载权重 */
    if (load_weights_from_sd() != 0) {
        xil_printf("FATAL: weight load failed\r\n");
        return 1;
    }

    /* 2. 建立 LayerWeight 映射 */
    fv_build_layer_weights(lw, FV_DDR_BASE);
    xil_printf("[Init] LayerWeight array built for %d layers\r\n", FV_NUM_LAYERS);

    /* 3. 初始化测试输入 (全零图) */
    for (int i = 0; i < 3 * 128 * 128; i++)
        test_input[i] = 0;

    int8_t *ping = (int8_t *)FV_FEAT_PING_BASE;
    int8_t *pong = (int8_t *)FV_FEAT_PONG_BASE;

    /* 4. 热身跑一次 (cache warm-up) */
    xil_printf("[Warmup] Running warmup...\r\n");
    fastvit_t8_infer(test_input, test_output, lw, ping, pong);
    xil_printf("[Warmup] Done.\r\n");

    /* 5. 计时推理 */
    xil_printf("[Infer] Starting timed inference...\r\n");
    XTime t0, t1;
    XTime_GetTime(&t0);
    int ret = fastvit_t8_infer(test_input, test_output, lw, ping, pong);
    XTime_GetTime(&t1);

    double elapsed_ms = (double)(t1 - t0) * 1000.0 / XPAR_CPU_CORTEXA9_0_CPU_CLK_FREQ_HZ;

    xil_printf("[Infer] Return: %d\r\n", ret);
    xil_printf("[Timing] Elapsed: %.1f ms\r\n", elapsed_ms);
    xil_printf("[Output] output[0..7]: ");
    for (int i = 0; i < 8; i++)
        xil_printf("%4d ", (int)test_output[i]);
    xil_printf("\r\n");

    /* 6. 简单验证: 全零输入应得非全零输出 (bias 效果) */
    int nonzero = 0;
    for (int i = 0; i < 768 * 4 * 4; i++)
        if (test_output[i] != 0) nonzero++;
    xil_printf("[Check] Non-zero outputs: %d / %d\r\n", nonzero, 768 * 4 * 4);

    xil_printf("=========================================\r\n");
    xil_printf("  DONE\r\n");
    xil_printf("=========================================\r\n");

    return 0;
}
