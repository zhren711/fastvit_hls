/*================================================================
 * fastvit_infer_v256.c -- Phase A checkpoint (2026-08-15), board test #1.
 *
 * Dispatch driven by tools/layer_descriptor_256.h, generated straight from
 * tools/layer_descriptor_256.json (Phase A step 2a, 0-diff self-checked
 * against the real ONNX DAG) -- not a re-derivation, the SAME artifact.
 * This is the first end-to-end test of "generator output -> real hardware
 * behavior", which verify_layer_dag.py's self-check alone cannot cover
 * (that check only proves the JSON is internally consistent with the ONNX
 * graph, see ZHR-8/91: the earlier Phase 0.8 coverage checker also
 * reported clean 0-diff while DW3's residual wiring was still wrong on
 * all 10 blocks, because it only checked shape/dispatch coverage, never
 * dataflow wiring).
 *
 * Deliberate scope for THIS driver: structural/shape fixes only (bugs
 * 1-3 -- correct 256x256 strides/shapes/dispatch coverage, straight from
 * ONNX, no hand-typed literals). Bug 4 (token_mixer/DW3's residual
 * operand) is NOT fixed here -- the correct wiring needs a 4th buffer
 * whose use as an Add operand is the exact pattern already shown to
 * silently fail on this hardware (ZHR-8 2026-08-15, defect 5, add_worker
 * write-back, unresolved). This driver keeps the OLD (wrong) residual --
 * DW7's output instead of token_mixer's raw output -- using only the
 * three addresses (ping/pong/temp) already proven safe as Add operands,
 * so this checkpoint isolates "how much do the structural fixes alone
 * improve accuracy", uncontaminated by a second unresolved issue. This is
 * a disclosed, temporary deviation from the descriptor's literal DAG
 * spec (which says DW3 feeds both DW7 and the Add directly), not a silent
 * regression.
 *
 * Weights/quantization params are UNCHANGED from Phase 0.8 (still the
 * uncalibrated default_act_scale=1/127 placeholder, no LayerScale gamma
 * folding) -- deliberately, so this measurement isolates the structural
 * fix's contribution before step 2b's numeric fixes are layered on.
 *
 * 用法: ./fastvit_infer_v256 <input.bin> <output.bin> [weights_dir]
 *   input.bin:  raw int8 [3,256,256]
 *   output.bin: raw int8 [768,8,8]
 *================================================================*/
#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "layer_descriptor_256.h"
#include "weights_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>

#define FV_DDR_BASE       0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define FV_FEAT_TEMP_BASE 0x12500000UL
#define FV_DMA_SIZE       0x06000000UL

static struct timespec _ts_base;
static int _ts_init = 0;
#define TSTEP(s) do { \
    struct timespec _now; \
    clock_gettime(CLOCK_MONOTONIC, &_now); \
    if (!_ts_init) { _ts_base = _now; _ts_init = 1; } \
    double _ms = (_now.tv_sec - _ts_base.tv_sec)*1000.0 \
               + (_now.tv_nsec - _ts_base.tv_nsec)/1.0e6; \
    fprintf(stderr, "[LayerTiming] %9.2f ms  %s\n", _ms, s); \
    fflush(stderr); \
} while(0)

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
            if (fread(ddr + FV_W_OFFSETS[i], 1, FV_WEIGHT_SIZES[i], f) != FV_WEIGHT_SIZES[i])
                fprintf(stderr, "short read: %s\n", path);
            fclose(f);
        }
        if (FV_BIAS_SIZES[i] > 0 && FV_BIAS_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_BIAS_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            if (fread(ddr + FV_B_OFFSETS[i], 1, FV_BIAS_SIZES[i], f) != FV_BIAS_SIZES[i])
                fprintf(stderr, "short read: %s\n", path);
            fclose(f);
        }
    }
    msync(dma_base_virt, FV_WEIGHT_TOTAL_BYTES, MS_SYNC);
    return 0;
}

/* ── Sigmoid LUT (unchanged from Phase 0.7/0.8, se_block scope only) ── */
static int8_t sigmoid_lut[256];
static void init_sigmoid_lut(void) {
    for (int i = 0; i < 256; i++) {
        float x = (i - 128) * (1.0f / 64.0f);
        float s = 1.0f / (1.0f + expf(-x));
        sigmoid_lut[i] = (int8_t)(s * 127.0f + 0.5f);
    }
}

static void se_block(
    int8_t *feat, int C, int H, int W,
    const int8_t *w_sq, const int32_t *b_sq, int C_sq, int shift_sq,
    const int8_t *w_ex, const int32_t *b_ex, int shift_ex)
{
    int sp = H * W;
    int8_t gap[1024], sq[64], ex[1024];
    for (int c = 0; c < C; c++) {
        int32_t s = 0;
        for (int p = 0; p < sp; p++) s += feat[c*sp+p];
        s /= sp;
        gap[c] = (int8_t)(s < -128 ? -128 : s > 127 ? 127 : s);
    }
    for (int co = 0; co < C_sq; co++) {
        int32_t a = b_sq ? b_sq[co] : 0;
        for (int ci = 0; ci < C; ci++) a += (int32_t)gap[ci]*w_sq[co*C+ci];
        a >>= shift_sq;
        sq[co] = (int8_t)(a < 0 ? 0 : a > 127 ? 127 : a);
    }
    for (int co = 0; co < C; co++) {
        int32_t a = b_ex ? b_ex[co] : 0;
        for (int ci = 0; ci < C_sq; ci++) a += (int32_t)sq[ci]*w_ex[co*C_sq+ci];
        a >>= shift_ex;
        ex[co] = (int8_t)(a < -128 ? -128 : a > 127 ? 127 : a);
    }
    for (int co = 0; co < C; co++) ex[co] = sigmoid_lut[(uint8_t)((int)ex[co] + 128)];
    for (int c = 0; c < C; c++) {
        int16_t sc = ex[c];
        for (int p = 0; p < sp; p++) {
            int32_t v = (int32_t)feat[c*sp+p]*sc >> 7;
            feat[c*sp+p] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    }
}

#define SWAP() do { int8_t *_t = cur; cur = nxt; nxt = _t; } while(0)

/* Same safe buffer choreography as the pre-bug4-fix Phase 0.8 rewrite:
 * residual = DW7(token_mixer(x)), NOT token_mixer(x) itself (bug 4, not
 * fixed this round -- see file header). Nets 0 SWAP()s across the whole
 * call (2 internal, identity-preserving), only ping/pong/temp used, in
 * their original proven-safe Add-operand roles. */
static void repmixer_block(
    int8_t **p_cur, int8_t **p_nxt, int8_t *temp,
    const LayerWeight *lw, int base_idx, const char *name)
{
    int8_t *cur = *p_cur, *nxt = *p_nxt;
    const FvLayerDesc256 *t_dw3 = &FV_DESC_256[base_idx + 0];
    const FvLayerDesc256 *t_dw7 = &FV_DESC_256[base_idx + 1];
    const FvLayerDesc256 *t_pw1 = &FV_DESC_256[base_idx + 2];
    const LayerWeight *w_dw3 = &lw[base_idx + 0];
    const LayerWeight *w_dw7 = &lw[base_idx + 1];
    const LayerWeight *w_pw1 = &lw[base_idx + 2];
    const LayerWeight *w_pw2 = &lw[base_idx + 3];
    int C = t_dw3->cin, H = t_dw3->h_in, W = t_dw3->w_in;
    int C_expand = t_pw1->cout;

    char _lbl[128];
    snprintf(_lbl, sizeof(_lbl), "  [%s] DW3 C=%d %dx%d", name, C, H, W); TSTEP(_lbl);
    fv_run_dwconv((uintptr_t)cur, w_dw3->w_addr, w_dw3->b_addr, (uintptr_t)temp,
                  C, H, W, t_dw3->k, t_dw3->k, t_dw3->stride, t_dw3->stride,
                  t_dw3->pad, t_dw3->pad, t_dw3->fpg, ACT_NONE, w_dw3->out_shift);
    snprintf(_lbl, sizeof(_lbl), "  [%s] DW7 C=%d %dx%d", name, C, H, W); TSTEP(_lbl);
    fv_run_dwconv((uintptr_t)temp, w_dw7->w_addr, w_dw7->b_addr, (uintptr_t)nxt,
                  C, H, W, t_dw7->k, t_dw7->k, t_dw7->stride, t_dw7->stride,
                  t_dw7->pad, t_dw7->pad, t_dw7->fpg, ACT_NONE, w_dw7->out_shift);
    SWAP();
    snprintf(_lbl, sizeof(_lbl), "  [%s] PW1 %d->%d %dx%d", name, C, C_expand, H, W); TSTEP(_lbl);
    fv_run_pwconv((uintptr_t)cur, w_pw1->w_addr, w_pw1->b_addr, (uintptr_t)nxt,
                  C, H, W, C_expand, ACT_NONE, w_pw1->out_shift);
    SWAP();
    snprintf(_lbl, sizeof(_lbl), "  [%s] GELU %d elem", name, C_expand*H*W); TSTEP(_lbl);
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, C_expand, H, W);
    snprintf(_lbl, sizeof(_lbl), "  [%s] PW2 %d->%d %dx%d", name, C_expand, C, H, W); TSTEP(_lbl);
    fv_run_pwconv((uintptr_t)cur, w_pw2->w_addr, w_pw2->b_addr, (uintptr_t)temp,
                  C_expand, H, W, C, ACT_NONE, w_pw2->out_shift);
    if (base_idx == 3) {
        FILE *f1 = fopen("/tmp/s1b0_residual_dw7out.bin", "wb");
        fwrite(nxt, 1, (size_t)C*H*W, f1); fclose(f1);
        FILE *f2 = fopen("/tmp/s1b0_convffn_pw2out.bin", "wb");
        fwrite(temp, 1, (size_t)C*H*W, f2); fclose(f2);
        FILE *f3 = fopen("/tmp/s1b0_cur_preadd.bin", "wb");
        fwrite(cur, 1, (size_t)C*H*W, f3); fclose(f3);
    }
    snprintf(_lbl, sizeof(_lbl), "  [%s] Add(residual=DW7out, bug4 NOT fixed) C=%d %dx%d", name, C, H, W); TSTEP(_lbl);
    fv_run_add((uintptr_t)nxt, (uintptr_t)temp, (uintptr_t)cur, C, H, W);
    if (base_idx == 3) {
        FILE *f4 = fopen("/tmp/s1b0_cur_postadd.bin", "wb");
        fwrite(cur, 1, (size_t)C*H*W, f4); fclose(f4);
        /* diagnostic: is this a missed-invalidate stale-ARM-cache-read
         * (matches the project's known Zynq HP non-coherence hazard),
         * not a genuine IP write failure? fv_run_add() already calls
         * fv_cache_invalidate(out,...) internally -- redundantly call it
         * AGAIN here with a generous size and re-read. If the value
         * CHANGES, the first invalidate wasn't effective. */
        uintptr_t cur_phys = FV_FEAT_PONG_BASE; /* known: cur==pong at S1B0 Add time */
        fv_cache_invalidate(cur_phys, (size_t)C*H*W);
        FILE *f5 = fopen("/tmp/s1b0_cur_postadd_reinvalidated.bin", "wb");
        fwrite(cur, 1, (size_t)C*H*W, f5); fclose(f5);
    }
    *p_cur = cur; *p_nxt = nxt;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <input.bin> <output.bin> [weights_dir]\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    const char *weights_dir = (argc > 3) ? argv[3] : "/home/root/weights_t8_pruned_p08";

    init_sigmoid_lut();

    if (fv_driver_init() != 0) return 1;
    if (dma_init() != 0) { fv_driver_exit(); return 1; }
    if (load_weights(weights_dir) != 0) { fv_driver_exit(); return 1; }

    static LayerWeight lw[FV_NUM_LAYERS];
    fv_build_layer_weights(lw, FV_DDR_BASE);

    int8_t *ping = (int8_t *)phys_to_virt(FV_FEAT_PING_BASE);
    int8_t *pong = (int8_t *)phys_to_virt(FV_FEAT_PONG_BASE);
    int8_t *temp = (int8_t *)phys_to_virt(FV_FEAT_TEMP_BASE);

    static int8_t test_input[3 * 256 * 256];
    static int8_t test_output[768 * 8 * 8];
    FILE *fin = fopen(in_path, "rb");
    if (!fin) { perror("open input"); return 1; }
    if (fread(test_input, 1, sizeof(test_input), fin) != sizeof(test_input))
        fprintf(stderr, "WARNING: short read on input\n");
    fclose(fin);

    int8_t *cur = ping, *nxt = pong;
    memcpy(cur, test_input, sizeof(test_input));

    TSTEP("start");

    /* Stem */
    fv_run_conv((uintptr_t)cur, lw[0].w_addr, lw[0].b_addr, (uintptr_t)nxt,
                FV_DESC_256[0].cin, FV_DESC_256[0].h_in, FV_DESC_256[0].w_in, FV_DESC_256[0].cout,
                FV_DESC_256[0].stride, FV_DESC_256[0].stride, FV_DESC_256[0].pad, FV_DESC_256[0].pad,
                ACT_NONE, lw[0].out_shift); SWAP();
    fv_run_dwconv((uintptr_t)cur, lw[1].w_addr, lw[1].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[1].cin, FV_DESC_256[1].h_in, FV_DESC_256[1].w_in,
                  FV_DESC_256[1].k, FV_DESC_256[1].k, FV_DESC_256[1].stride, FV_DESC_256[1].stride,
                  FV_DESC_256[1].pad, FV_DESC_256[1].pad, FV_DESC_256[1].fpg, ACT_NONE, lw[1].out_shift); SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_DESC_256[1].cout,
                FV_DESC_256[1].h_in / FV_DESC_256[1].stride, FV_DESC_256[1].w_in / FV_DESC_256[1].stride);
    fv_run_pwconv((uintptr_t)cur, lw[2].w_addr, lw[2].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[2].cin, FV_DESC_256[2].h_in, FV_DESC_256[2].w_in, FV_DESC_256[2].cout,
                  ACT_NONE, lw[2].out_shift); SWAP();

    /* Stage1 */
    repmixer_block(&cur, &nxt, temp, lw, 3, "S1B0");
    repmixer_block(&cur, &nxt, temp, lw, 7, "S1B1");

    /* Trans1 */
    fv_run_dwconv((uintptr_t)cur, lw[11].w_addr, lw[11].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[11].cin, FV_DESC_256[11].h_in, FV_DESC_256[11].w_in,
                  FV_DESC_256[11].k, FV_DESC_256[11].k, FV_DESC_256[11].stride, FV_DESC_256[11].stride,
                  FV_DESC_256[11].pad, FV_DESC_256[11].pad, FV_DESC_256[11].fpg, ACT_NONE, lw[11].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_DESC_256[11].cout,
                FV_DESC_256[11].h_in / FV_DESC_256[11].stride, FV_DESC_256[11].w_in / FV_DESC_256[11].stride);
    fv_run_pwconv((uintptr_t)cur, lw[12].w_addr, lw[12].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[12].cin, FV_DESC_256[12].h_in, FV_DESC_256[12].w_in, FV_DESC_256[12].cout,
                  ACT_NONE, lw[12].out_shift); SWAP();

    /* Stage2 */
    repmixer_block(&cur, &nxt, temp, lw, 13, "S2B0");
    repmixer_block(&cur, &nxt, temp, lw, 17, "S2B1");

    /* Trans2 */
    fv_run_dwconv((uintptr_t)cur, lw[21].w_addr, lw[21].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[21].cin, FV_DESC_256[21].h_in, FV_DESC_256[21].w_in,
                  FV_DESC_256[21].k, FV_DESC_256[21].k, FV_DESC_256[21].stride, FV_DESC_256[21].stride,
                  FV_DESC_256[21].pad, FV_DESC_256[21].pad, FV_DESC_256[21].fpg, ACT_NONE, lw[21].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_DESC_256[21].cout,
                FV_DESC_256[21].h_in / FV_DESC_256[21].stride, FV_DESC_256[21].w_in / FV_DESC_256[21].stride);
    fv_run_pwconv((uintptr_t)cur, lw[22].w_addr, lw[22].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[22].cin, FV_DESC_256[22].h_in, FV_DESC_256[22].w_in, FV_DESC_256[22].cout,
                  ACT_NONE, lw[22].out_shift); SWAP();

    /* Stage3 */
    repmixer_block(&cur, &nxt, temp, lw, 23, "S3B0");
    repmixer_block(&cur, &nxt, temp, lw, 27, "S3B1");
    repmixer_block(&cur, &nxt, temp, lw, 31, "S3B2");
    repmixer_block(&cur, &nxt, temp, lw, 35, "S3B3");

    /* Trans3 */
    fv_run_dwconv((uintptr_t)cur, lw[39].w_addr, lw[39].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[39].cin, FV_DESC_256[39].h_in, FV_DESC_256[39].w_in,
                  FV_DESC_256[39].k, FV_DESC_256[39].k, FV_DESC_256[39].stride, FV_DESC_256[39].stride,
                  FV_DESC_256[39].pad, FV_DESC_256[39].pad, FV_DESC_256[39].fpg, ACT_NONE, lw[39].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_DESC_256[39].cout,
                FV_DESC_256[39].h_in / FV_DESC_256[39].stride, FV_DESC_256[39].w_in / FV_DESC_256[39].stride);
    fv_run_pwconv((uintptr_t)cur, lw[40].w_addr, lw[40].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[40].cin, FV_DESC_256[40].h_in, FV_DESC_256[40].w_in, FV_DESC_256[40].cout,
                  ACT_NONE, lw[40].out_shift); SWAP();

    /* Stage4 */
    repmixer_block(&cur, &nxt, temp, lw, 41, "S4B0");
    repmixer_block(&cur, &nxt, temp, lw, 45, "S4B1");

    /* FinalDW */
    fv_run_dwconv((uintptr_t)cur, lw[49].w_addr, lw[49].b_addr, (uintptr_t)nxt,
                  FV_DESC_256[49].cin, FV_DESC_256[49].h_in, FV_DESC_256[49].w_in,
                  FV_DESC_256[49].k, FV_DESC_256[49].k, FV_DESC_256[49].stride, FV_DESC_256[49].stride,
                  FV_DESC_256[49].pad, FV_DESC_256[49].pad, FV_DESC_256[49].fpg, ACT_NONE, lw[49].out_shift);
    SWAP();

    /* SE (ARM) */
    TSTEP("SE block C=768 8x8 [ARM]");
    {
        int8_t *ddr_virt = ping - (FV_FEAT_PING_BASE - FV_DDR_BASE);
        se_block(cur, 768, 8, 8,
            (const int8_t  *)(ddr_virt + (lw[50].w_addr - FV_DDR_BASE)),
            (const int32_t *)(void*)(lw[50].b_addr ? ddr_virt + (lw[50].b_addr - FV_DDR_BASE) : NULL),
            48, lw[50].out_shift,
            (const int8_t  *)(ddr_virt + (lw[51].w_addr - FV_DDR_BASE)),
            (const int32_t *)(void*)(lw[51].b_addr ? ddr_virt + (lw[51].b_addr - FV_DDR_BASE) : NULL),
            lw[51].out_shift);
    }

    TSTEP("done");
    memcpy(test_output, cur, sizeof(test_output));

    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror("open output"); return 1; }
    fwrite(test_output, 1, sizeof(test_output), fout);
    fclose(fout);

    int nonzero = 0;
    for (size_t i = 0; i < sizeof(test_output); i++) if (test_output[i] != 0) nonzero++;
    fprintf(stderr, "[fastvit_infer_v256] %s -> %s  nonzero=%d/%zu\n",
            in_path, out_path, nonzero, sizeof(test_output));

    fv_driver_exit();
    return 0;
}
