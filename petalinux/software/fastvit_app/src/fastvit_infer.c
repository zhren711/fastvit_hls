/**
 * fastvit_infer.c v5 — ALL operators on FPGA
 *
 * dwconv_ip v9.0 支持 fpg (expand factor):
 *   fpg=1: 标准 DW (CHout = CHin)
 *   fpg=2: grouped-expand DW (CHout = 2*CHin)
 *
 * 所有 4 个 ARM transition_dwconv 替换为 FPGA fpg=2 DWConv:
 *   输入: [Cin, H, W]  →  输出: [2*Cin, H/2, W/2]
 *   权重: 原始 weight 文件，无需拆分
 *
 * 保留 SE block (ARM, 2.8ms, negligible)
 */

#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "layer_topology.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ── timing macro ─────────────────────────────────── */
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

/* ── Sigmoid LUT ─────────────────────────────────── */
static int8_t sigmoid_lut[256];
static int sigmoid_lut_ready = 0;
static void init_sigmoid_lut(void) {
    if (sigmoid_lut_ready) return;
    for (int i = 0; i < 256; i++) {
        float x = (i - 128) * (1.0f / 64.0f);
        float s = 1.0f / (1.0f + expf(-x));
        sigmoid_lut[i] = (int8_t)(s * 127.0f + 0.5f);
    }
    sigmoid_lut_ready = 1;
}

/* ── SE block (ARM, only 2.8ms, keep as-is) ────────── */
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
    /* Phase 0.7 step 1 fix (2026-08-13): sigmoid_lut was built as
     * x=(i-128)/64 (index 128 = x=0 = sigmoid 0.5), so a correct lookup
     * needs index = ex[co]+128 (int8 -> unsigned OFFSET). The old
     * (uint8_t)ex[co] is a raw bit-reinterpret CAST instead (ex=0->0,
     * ex=-128->128), which swaps the LUT's two halves -- confirmed via
     * Phase 0.6 debug instrumentation and code review, see ZHR-8. */
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

static char _ts_label[128];
#define TSTEP_FMT(...) do { \
    snprintf(_ts_label, sizeof(_ts_label), __VA_ARGS__); \
    TSTEP(_ts_label); \
} while(0)

/* Phase 0.8 (2026-08-15): parameterized entirely by FV_TOPO[base_idx..+3]
 * (auto-generated from the real ONNX graph, tools/gen_layer_topology.py) --
 * no more hand-typed C/H/W/K/stride/pad and no more has_dw3 special case.
 * Bug 3 (Phase 0.7 step 10: Stage1 block0's token_mixer conv, layer_0003,
 * was loaded but never dispatched) is fixed structurally here: the real
 * ONNX graph confirms all 10 RepMixer blocks have an identical 4-conv
 * layout (token_mixer dw3, mlp.conv dw7, mlp.fc1 pw, mlp.fc2 pw), so this
 * function always dispatches all 4 -- there is no longer a code path that
 * can skip one. */
static void repmixer_block(
    int8_t **p_cur, int8_t **p_nxt, int8_t *temp,
    const LayerWeight *lw, int base_idx, const char *name)
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

    TSTEP_FMT("  [%s] DW3 C=%d %dx%d", name, C, H, W);
    fv_run_dwconv((uintptr_t)cur, w_dw3->w_addr, w_dw3->b_addr, (uintptr_t)temp,
                  C, H, W, t_dw3->k, t_dw3->k, t_dw3->stride, t_dw3->stride,
                  t_dw3->pad, t_dw3->pad, t_dw3->fpg, ACT_NONE, w_dw3->out_shift);
    TSTEP_FMT("  [%s] DW7 C=%d %dx%d", name, C, H, W);
    fv_run_dwconv((uintptr_t)temp, w_dw7->w_addr, w_dw7->b_addr, (uintptr_t)nxt,
                  C, H, W, t_dw7->k, t_dw7->k, t_dw7->stride, t_dw7->stride,
                  t_dw7->pad, t_dw7->pad, t_dw7->fpg, ACT_NONE, w_dw7->out_shift);
    SWAP();
    TSTEP_FMT("  [%s] PW1 %d->%d %dx%d", name, C, C_expand, H, W);
    fv_run_pwconv((uintptr_t)cur, w_pw1->w_addr, w_pw1->b_addr, (uintptr_t)nxt,
                  C, H, W, C_expand, ACT_NONE, w_pw1->out_shift);
    SWAP();
    TSTEP_FMT("  [%s] GELU %d elem", name, C_expand * H * W);
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, C_expand, H, W);
    TSTEP_FMT("  [%s] PW2 %d->%d %dx%d", name, C_expand, C, H, W);
    fv_run_pwconv((uintptr_t)cur, w_pw2->w_addr, w_pw2->b_addr, (uintptr_t)temp,
                  C_expand, H, W, C, ACT_NONE, w_pw2->out_shift);
    TSTEP_FMT("  [%s] Add C=%d %dx%d", name, C, H, W);
    fv_run_add((uintptr_t)nxt, (uintptr_t)temp, (uintptr_t)cur, C, H, W);
    TSTEP_FMT("  [%s] done", name);
    *p_cur = cur; *p_nxt = nxt;
}

/* ═══════════════════════════════════════════════════════════
 * fastvit_t8_infer — all transitions on FPGA via fpg=2 DWConv
 * ═══════════════════════════════════════════════════════════ */
int fastvit_t8_infer(
    const int8_t      *input,
    int8_t            *output,
    const LayerWeight *lw,
    int8_t            *ping,
    int8_t            *pong)
{
    init_sigmoid_lut();
    _ts_init = 0;

    int8_t *cur  = ping;
    int8_t *nxt  = pong;
    int8_t *temp = (int8_t *)FV_FEAT_TEMP_BASE;

    TSTEP("start");
    memcpy(cur, input, 3 * 128 * 128);

    /* ─── STEM ─────────────────────────────────────────── */
    TSTEP("Stem: Conv3x3 3->48 s=2");
    fv_run_conv((uintptr_t)cur, lw[0].w_addr, lw[0].b_addr, (uintptr_t)nxt,
                FV_TOPO[0].cin, FV_TOPO[0].h_in, FV_TOPO[0].w_in, FV_TOPO[0].cout,
                FV_TOPO[0].stride, FV_TOPO[0].stride, FV_TOPO[0].pad, FV_TOPO[0].pad,
                ACT_NONE, lw[0].out_shift); SWAP();
    TSTEP("Stem: DW3x3 48 s=2 + GELU");
    fv_run_dwconv((uintptr_t)cur, lw[1].w_addr, lw[1].b_addr, (uintptr_t)nxt,
                  FV_TOPO[1].cin, FV_TOPO[1].h_in, FV_TOPO[1].w_in,
                  FV_TOPO[1].k, FV_TOPO[1].k, FV_TOPO[1].stride, FV_TOPO[1].stride,
                  FV_TOPO[1].pad, FV_TOPO[1].pad, FV_TOPO[1].fpg, ACT_NONE, lw[1].out_shift); SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[1].cout,
                FV_TOPO[1].h_in / FV_TOPO[1].stride, FV_TOPO[1].w_in / FV_TOPO[1].stride);
    TSTEP("Stem: PW 48->48");
    fv_run_pwconv((uintptr_t)cur, lw[2].w_addr, lw[2].b_addr, (uintptr_t)nxt,
                  FV_TOPO[2].cin, FV_TOPO[2].h_in, FV_TOPO[2].w_in, FV_TOPO[2].cout,
                  ACT_NONE, lw[2].out_shift); SWAP();

    /* ─── STAGE 1 (C=48, real 32×32 -- Phase 0.7 step 10 bug 1 fixed:
     * Stem's 2nd conv now runs at its real stride=(2,2) instead of the
     * hardcoded (1,1), so every stage's spatial size below is 2x smaller
     * per dimension than the old (buggy) trace labels claimed) ────── */
    TSTEP("Stage1 blk0 RepMixer C=48 32x32");
    repmixer_block(&cur, &nxt, temp, lw, 3, "S1B0");
    TSTEP("Stage1 blk1 RepMixer C=48 32x32");
    repmixer_block(&cur, &nxt, temp, lw, 7, "S1B1");

    /* ─── TRANSITION 1: FPGA fpg=2 (48→96, 32→16, K=7) ── */
    TSTEP("Trans1 FPGA DW7 fpg=2 48->96 32->16");
    fv_run_dwconv((uintptr_t)cur, lw[11].w_addr, lw[11].b_addr, (uintptr_t)nxt,
                  FV_TOPO[11].cin, FV_TOPO[11].h_in, FV_TOPO[11].w_in,
                  FV_TOPO[11].k, FV_TOPO[11].k, FV_TOPO[11].stride, FV_TOPO[11].stride,
                  FV_TOPO[11].pad, FV_TOPO[11].pad, FV_TOPO[11].fpg, ACT_NONE, lw[11].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[11].cout,
                FV_TOPO[11].h_in / FV_TOPO[11].stride, FV_TOPO[11].w_in / FV_TOPO[11].stride);
    TSTEP("Trans1 PW 96->96");
    fv_run_pwconv((uintptr_t)cur, lw[12].w_addr, lw[12].b_addr, (uintptr_t)nxt,
                  FV_TOPO[12].cin, FV_TOPO[12].h_in, FV_TOPO[12].w_in, FV_TOPO[12].cout,
                  ACT_NONE, lw[12].out_shift); SWAP();

    /* ─── STAGE 2 (C=96, 16×16) ────────────────────────── */
    TSTEP("Stage2 blk0 RepMixer C=96 16x16");
    repmixer_block(&cur, &nxt, temp, lw, 13, "S2B0");  /* PW1 pruned 288->240 (weights_t8_pruned) */
    TSTEP("Stage2 blk1 RepMixer C=96 16x16");
    repmixer_block(&cur, &nxt, temp, lw, 17, "S2B1");

    /* ─── TRANSITION 2: FPGA fpg=2 (96→192, 16→8, K=7) ─ */
    TSTEP("Trans2 FPGA DW7 fpg=2 96->192 16->8");
    fv_run_dwconv((uintptr_t)cur, lw[21].w_addr, lw[21].b_addr, (uintptr_t)nxt,
                  FV_TOPO[21].cin, FV_TOPO[21].h_in, FV_TOPO[21].w_in,
                  FV_TOPO[21].k, FV_TOPO[21].k, FV_TOPO[21].stride, FV_TOPO[21].stride,
                  FV_TOPO[21].pad, FV_TOPO[21].pad, FV_TOPO[21].fpg, ACT_NONE, lw[21].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[21].cout,
                FV_TOPO[21].h_in / FV_TOPO[21].stride, FV_TOPO[21].w_in / FV_TOPO[21].stride);
    TSTEP("Trans2 PW 192->192");
    fv_run_pwconv((uintptr_t)cur, lw[22].w_addr, lw[22].b_addr, (uintptr_t)nxt,
                  FV_TOPO[22].cin, FV_TOPO[22].h_in, FV_TOPO[22].w_in, FV_TOPO[22].cout,
                  ACT_NONE, lw[22].out_shift); SWAP();

    /* ─── STAGE 3 (C=192, 8×8, 4 blocks) ─────────────── */
    TSTEP("Stage3 blk0 RepMixer C=192 8x8");
    repmixer_block(&cur, &nxt, temp, lw, 23, "S3B0");
    TSTEP("Stage3 blk1 RepMixer C=192 8x8");
    repmixer_block(&cur, &nxt, temp, lw, 27, "S3B1");  /* PW1 pruned 576->480 (weights_t8_pruned) */
    TSTEP("Stage3 blk2 RepMixer C=192 8x8");
    repmixer_block(&cur, &nxt, temp, lw, 31, "S3B2");
    TSTEP("Stage3 blk3 RepMixer C=192 8x8");
    repmixer_block(&cur, &nxt, temp, lw, 35, "S3B3");

    /* ─── TRANSITION 3: FPGA fpg=2 (192→384, 8→4, K=7) ─ */
    TSTEP("Trans3 FPGA DW7 fpg=2 192->384 8->4");
    fv_run_dwconv((uintptr_t)cur, lw[39].w_addr, lw[39].b_addr, (uintptr_t)nxt,
                  FV_TOPO[39].cin, FV_TOPO[39].h_in, FV_TOPO[39].w_in,
                  FV_TOPO[39].k, FV_TOPO[39].k, FV_TOPO[39].stride, FV_TOPO[39].stride,
                  FV_TOPO[39].pad, FV_TOPO[39].pad, FV_TOPO[39].fpg, ACT_NONE, lw[39].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, FV_TOPO[39].cout,
                FV_TOPO[39].h_in / FV_TOPO[39].stride, FV_TOPO[39].w_in / FV_TOPO[39].stride);
    TSTEP("Trans3 PW 384->384");
    fv_run_pwconv((uintptr_t)cur, lw[40].w_addr, lw[40].b_addr, (uintptr_t)nxt,
                  FV_TOPO[40].cin, FV_TOPO[40].h_in, FV_TOPO[40].w_in, FV_TOPO[40].cout,
                  ACT_NONE, lw[40].out_shift); SWAP();

    /* ─── STAGE 4 (C=384, 4×4, 2 blocks) ───────────────── */
    TSTEP("Stage4 blk0 RepMixer C=384 4x4");
    repmixer_block(&cur, &nxt, temp, lw, 41, "S4B0");
    TSTEP("Stage4 blk1 RepMixer C=384 4x4");
    repmixer_block(&cur, &nxt, temp, lw, 45, "S4B1");  /* PW1 pruned 1152->960 (weights_t8_pruned) */

    /* ─── FINAL DW: FPGA fpg=2 (384→768, K=3, real stride=(1,1) --
     * Phase 0.7 step 10 bug 2 fixed: Stage4 is already the final spatial
     * size (4x4), FinalDW does NOT downsample; the old hardcoded (2,2)
     * spuriously shrank it to a coincidentally-matching 4x4 only because
     * bug 1 had inflated every preceding stage 2x too large.) ──────── */
    TSTEP("FinalDW FPGA DW3 fpg=2 384->768 4x4 s=1");
    fv_run_dwconv((uintptr_t)cur, lw[49].w_addr, lw[49].b_addr, (uintptr_t)nxt,
                  FV_TOPO[49].cin, FV_TOPO[49].h_in, FV_TOPO[49].w_in,
                  FV_TOPO[49].k, FV_TOPO[49].k, FV_TOPO[49].stride, FV_TOPO[49].stride,
                  FV_TOPO[49].pad, FV_TOPO[49].pad, FV_TOPO[49].fpg, ACT_NONE, lw[49].out_shift);
    SWAP();

    /* ─── SE block (ARM, 2.8ms) ──────────────────────────── */
    TSTEP("SE block C=768 4x4 [ARM]");
    /* lw[].w_addr/b_addr are PHYSICAL addresses (built with FV_DDR_BASE in
     * main.c, "使用物理地址供 IP 访问" -- correct for the fv_run_*() FPGA
     * calls, which write them straight into AXI pointer registers). But
     * se_block() runs on the CPU and dereferences its weight pointers
     * directly, so it needs VIRTUAL addresses -- convert via the `ping`
     * pointer (already a CPU-valid virtual pointer for FV_FEAT_PING_BASE,
     * passed into this function), same DMA mapping as the weights region. */
    {
        int8_t *ddr_virt = ping - (FV_FEAT_PING_BASE - FV_DDR_BASE);
        se_block(cur, 768, 4, 4,
            (const int8_t  *)(ddr_virt + (lw[50].w_addr - FV_DDR_BASE)),
            (const int32_t *)(void*)(lw[50].b_addr ? ddr_virt + (lw[50].b_addr - FV_DDR_BASE) : NULL),
            48, lw[50].out_shift,
            (const int8_t  *)(ddr_virt + (lw[51].w_addr - FV_DDR_BASE)),
            (const int32_t *)(void*)(lw[51].b_addr ? ddr_virt + (lw[51].b_addr - FV_DDR_BASE) : NULL),
            lw[51].out_shift);
    }

    TSTEP("done");
    memcpy(output, cur, 768 * 4 * 4);
    return 0;
}
