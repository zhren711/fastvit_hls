/*================================================================
 * fastvit_infer_checkpoint_dump.c -- Phase 0.8 step 5 board-side tool.
 *
 * Standalone variant of fastvit_infer.c that dumps the FULL int8 feature
 * tensor (not just min/max/mean) at 6 checkpoints, for segmented
 * hardware-vs-ONNX cosine comparison (this project has never done a
 * layer-by-layer hardware-vs-float comparison before, only aggregate
 * end-to-end and hardware-vs-hardware). Reuses fastvit_driver.c's
 * fv_run_*() facade (cache flush/invalidate already handled internally
 * per-call, see CLAUDE.md) and layer_topology.h (same ONNX-derived
 * topology table as the fixed production fastvit_infer.c) -- this file
 * is NOT the production driver, it is a read-only instrumented copy of
 * its control flow, same convention as fastvit_infer_full_trace.c /
 * fastvit_infer_se_debug.c.
 *
 * Checkpoints (raw int8, written as flat binary, CHW layout):
 *   ckpt_stem.bin     [48,32,32]   after Stem (before Stage1)
 *   ckpt_stage1.bin   [48,32,32]   after Stage1 (2 blocks)
 *   ckpt_stage2.bin   [96,16,16]   after Stage2 (2 blocks)
 *   ckpt_stage3.bin   [192,8,8]    after Stage3 (4 blocks)
 *   ckpt_stage4.bin   [384,4,4]    after Stage4 (2 blocks)
 *   ckpt_finaldw.bin  [768,4,4]    after FinalDW (before SE)
 *
 * 用法: ./checkpoint_dump_infer <input.bin> <dump_dir> [weights_dir]
 *================================================================*/
#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "layer_topology.h"
#include "weights_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define FV_DDR_BASE       0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define FV_FEAT_TEMP_BASE 0x12500000UL
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
            if (fread(ddr + FV_W_OFFSETS[i], 1, FV_WEIGHT_SIZES[i], f) != FV_WEIGHT_SIZES[i]) {
                fprintf(stderr, "short read: %s\n", path);
            }
            fclose(f);
        }
        if (FV_BIAS_SIZES[i] > 0 && FV_BIAS_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_BIAS_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            if (fread(ddr + FV_B_OFFSETS[i], 1, FV_BIAS_SIZES[i], f) != FV_BIAS_SIZES[i]) {
                fprintf(stderr, "short read: %s\n", path);
            }
            fclose(f);
        }
    }
    msync(dma_base_virt, FV_WEIGHT_TOTAL_BYTES, MS_SYNC);
    return 0;
}

static char g_dump_dir[256];

static void dump_tensor(const int8_t *buf, int C, int H, int W, const char *tag) {
    char path[512];
    snprintf(path, sizeof(path), "%s/ckpt_%s.bin", g_dump_dir, tag);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(buf, 1, (size_t)C * H * W, f);
    fclose(f);
    fprintf(stderr, "[dump] %s  [%d,%d,%d]\n", path, C, H, W);
}

#define SWAP() do { int8_t *_t = cur; cur = nxt; nxt = _t; } while(0)

/* identical structure to repmixer_block() in fastvit_infer.c -- see that
 * file for the buffer-choreography rationale (token_mixer(X) residual,
 * not X itself, per RepMixer's fused reparam_conv). */
static void repmixer_block(
    int8_t **p_cur, int8_t **p_nxt, int8_t *temp,
    const LayerWeight *lw, int base_idx)
{
    int8_t *cur = *p_cur, *nxt = *p_nxt;
    const FvLayerTopo *t_dw3 = &FV_TOPO[base_idx + 0];
    const FvLayerTopo *t_dw7 = &FV_TOPO[base_idx + 1];
    const FvLayerTopo *t_pw1 = &FV_TOPO[base_idx + 2];
    const LayerWeight *w_dw3 = &lw[base_idx + 0];
    const LayerWeight *w_dw7 = &lw[base_idx + 1];
    const LayerWeight *w_pw1 = &lw[base_idx + 2];
    const LayerWeight *w_pw2 = &lw[base_idx + 3];
    int C = t_dw3->cin, H = t_dw3->h_in, W = t_dw3->w_in;
    int C_expand = t_pw1->cout;

    fv_run_dwconv((uintptr_t)cur, w_dw3->w_addr, w_dw3->b_addr, (uintptr_t)temp,
                  C, H, W, t_dw3->k, t_dw3->k, t_dw3->stride, t_dw3->stride,
                  t_dw3->pad, t_dw3->pad, t_dw3->fpg, ACT_NONE, w_dw3->out_shift);
    fv_run_dwconv((uintptr_t)temp, w_dw7->w_addr, w_dw7->b_addr, (uintptr_t)nxt,
                  C, H, W, t_dw7->k, t_dw7->k, t_dw7->stride, t_dw7->stride,
                  t_dw7->pad, t_dw7->pad, t_dw7->fpg, ACT_NONE, w_dw7->out_shift);
    SWAP();
    fv_run_pwconv((uintptr_t)cur, w_pw1->w_addr, w_pw1->b_addr, (uintptr_t)nxt,
                  C, H, W, C_expand, ACT_NONE, w_pw1->out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, C_expand, H, W);
    fv_run_pwconv((uintptr_t)cur, w_pw2->w_addr, w_pw2->b_addr, (uintptr_t)temp,
                  C_expand, H, W, C, ACT_NONE, w_pw2->out_shift);
    fv_run_add((uintptr_t)nxt, (uintptr_t)temp, (uintptr_t)cur, C, H, W);
    *p_cur = cur; *p_nxt = nxt;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.bin> <dump_dir> [weights_dir]\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", argv[2]);
    const char *weights_dir = (argc > 3) ? argv[3] : "/home/root/weights_t8_pruned_p08";

    if (fv_driver_init() != 0) return 1;
    if (dma_init() != 0) { fv_driver_exit(); return 1; }
    if (load_weights(weights_dir) != 0) { fv_driver_exit(); return 1; }

    static LayerWeight lw[FV_NUM_LAYERS];
    fv_build_layer_weights(lw, FV_DDR_BASE);

    int8_t *ping = (int8_t *)phys_to_virt(FV_FEAT_PING_BASE);
    int8_t *pong = (int8_t *)phys_to_virt(FV_FEAT_PONG_BASE);
    int8_t *temp = (int8_t *)phys_to_virt(FV_FEAT_TEMP_BASE);

    static int8_t test_input[3 * 128 * 128];
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror("open input"); return 1; }
    if (fread(test_input, 1, sizeof(test_input), fin) != sizeof(test_input)) {
        fprintf(stderr, "WARNING: short read on input\n");
    }
    fclose(fin);

    int8_t *cur = ping, *nxt = pong;
    memcpy(cur, test_input, sizeof(test_input));

    /* Stem */
    fv_run_conv((uintptr_t)cur, lw[0].w_addr, lw[0].b_addr, (uintptr_t)nxt,
                FV_TOPO[0].cin, FV_TOPO[0].h_in, FV_TOPO[0].w_in, FV_TOPO[0].cout,
                FV_TOPO[0].stride, FV_TOPO[0].stride, FV_TOPO[0].pad, FV_TOPO[0].pad,
                ACT_NONE, lw[0].out_shift); SWAP();
    fv_run_dwconv((uintptr_t)cur, lw[1].w_addr, lw[1].b_addr, (uintptr_t)nxt,
                  FV_TOPO[1].cin, FV_TOPO[1].h_in, FV_TOPO[1].w_in,
                  FV_TOPO[1].k, FV_TOPO[1].k, FV_TOPO[1].stride, FV_TOPO[1].stride,
                  FV_TOPO[1].pad, FV_TOPO[1].pad, FV_TOPO[1].fpg, ACT_NONE, lw[1].out_shift); SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[1].cout,
                FV_TOPO[1].h_in / FV_TOPO[1].stride, FV_TOPO[1].w_in / FV_TOPO[1].stride);
    fv_run_pwconv((uintptr_t)cur, lw[2].w_addr, lw[2].b_addr, (uintptr_t)nxt,
                  FV_TOPO[2].cin, FV_TOPO[2].h_in, FV_TOPO[2].w_in, FV_TOPO[2].cout,
                  ACT_NONE, lw[2].out_shift); SWAP();
    dump_tensor(cur, FV_TOPO[2].cout, FV_TOPO[2].h_in, FV_TOPO[2].w_in, "stem");

    /* Stage1 */
    repmixer_block(&cur, &nxt, temp, lw, 3);
    repmixer_block(&cur, &nxt, temp, lw, 7);
    dump_tensor(cur, 48, 32, 32, "stage1");

    /* Trans1 */
    fv_run_dwconv((uintptr_t)cur, lw[11].w_addr, lw[11].b_addr, (uintptr_t)nxt,
                  FV_TOPO[11].cin, FV_TOPO[11].h_in, FV_TOPO[11].w_in,
                  FV_TOPO[11].k, FV_TOPO[11].k, FV_TOPO[11].stride, FV_TOPO[11].stride,
                  FV_TOPO[11].pad, FV_TOPO[11].pad, FV_TOPO[11].fpg, ACT_NONE, lw[11].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[11].cout,
                FV_TOPO[11].h_in / FV_TOPO[11].stride, FV_TOPO[11].w_in / FV_TOPO[11].stride);
    fv_run_pwconv((uintptr_t)cur, lw[12].w_addr, lw[12].b_addr, (uintptr_t)nxt,
                  FV_TOPO[12].cin, FV_TOPO[12].h_in, FV_TOPO[12].w_in, FV_TOPO[12].cout,
                  ACT_NONE, lw[12].out_shift); SWAP();

    /* Stage2 */
    repmixer_block(&cur, &nxt, temp, lw, 13);
    repmixer_block(&cur, &nxt, temp, lw, 17);
    dump_tensor(cur, 96, 16, 16, "stage2");

    /* Trans2 */
    fv_run_dwconv((uintptr_t)cur, lw[21].w_addr, lw[21].b_addr, (uintptr_t)nxt,
                  FV_TOPO[21].cin, FV_TOPO[21].h_in, FV_TOPO[21].w_in,
                  FV_TOPO[21].k, FV_TOPO[21].k, FV_TOPO[21].stride, FV_TOPO[21].stride,
                  FV_TOPO[21].pad, FV_TOPO[21].pad, FV_TOPO[21].fpg, ACT_NONE, lw[21].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[21].cout,
                FV_TOPO[21].h_in / FV_TOPO[21].stride, FV_TOPO[21].w_in / FV_TOPO[21].stride);
    fv_run_pwconv((uintptr_t)cur, lw[22].w_addr, lw[22].b_addr, (uintptr_t)nxt,
                  FV_TOPO[22].cin, FV_TOPO[22].h_in, FV_TOPO[22].w_in, FV_TOPO[22].cout,
                  ACT_NONE, lw[22].out_shift); SWAP();

    /* Stage3 */
    repmixer_block(&cur, &nxt, temp, lw, 23);
    repmixer_block(&cur, &nxt, temp, lw, 27);
    repmixer_block(&cur, &nxt, temp, lw, 31);
    repmixer_block(&cur, &nxt, temp, lw, 35);
    dump_tensor(cur, 192, 8, 8, "stage3");

    /* Trans3 */
    fv_run_dwconv((uintptr_t)cur, lw[39].w_addr, lw[39].b_addr, (uintptr_t)nxt,
                  FV_TOPO[39].cin, FV_TOPO[39].h_in, FV_TOPO[39].w_in,
                  FV_TOPO[39].k, FV_TOPO[39].k, FV_TOPO[39].stride, FV_TOPO[39].stride,
                  FV_TOPO[39].pad, FV_TOPO[39].pad, FV_TOPO[39].fpg, ACT_NONE, lw[39].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[39].cout,
                FV_TOPO[39].h_in / FV_TOPO[39].stride, FV_TOPO[39].w_in / FV_TOPO[39].stride);
    fv_run_pwconv((uintptr_t)cur, lw[40].w_addr, lw[40].b_addr, (uintptr_t)nxt,
                  FV_TOPO[40].cin, FV_TOPO[40].h_in, FV_TOPO[40].w_in, FV_TOPO[40].cout,
                  ACT_NONE, lw[40].out_shift); SWAP();

    /* Stage4 */
    repmixer_block(&cur, &nxt, temp, lw, 41);
    repmixer_block(&cur, &nxt, temp, lw, 45);
    dump_tensor(cur, 384, 4, 4, "stage4");

    /* FinalDW */
    fv_run_dwconv((uintptr_t)cur, lw[49].w_addr, lw[49].b_addr, (uintptr_t)nxt,
                  FV_TOPO[49].cin, FV_TOPO[49].h_in, FV_TOPO[49].w_in,
                  FV_TOPO[49].k, FV_TOPO[49].k, FV_TOPO[49].stride, FV_TOPO[49].stride,
                  FV_TOPO[49].pad, FV_TOPO[49].pad, FV_TOPO[49].fpg, ACT_NONE, lw[49].out_shift);
    SWAP();
    dump_tensor(cur, 768, 4, 4, "finaldw");

    fv_driver_exit();
    return 0;
}
