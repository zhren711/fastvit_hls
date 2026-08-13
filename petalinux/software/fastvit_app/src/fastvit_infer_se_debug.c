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

/* ── SE block (ARM) -- DEBUG VARIANT, not the production copy ──────
 * Phase 0.6: added stat prints for gap/sq/ex(pre-sigmoid)/sc(post-
 * sigmoid)/feat(before SE), and an env-var-gated fix for a suspected
 * sigmoid_lut indexing bug (see below), to test the hypothesis without
 * touching the real fastvit_infer.c that main.c / the production
 * fastvit_infer_v18gelu binary link against. */
#include <stdlib.h>
static int8_t se_min(const int8_t *a, int n) { int8_t m=a[0]; for(int i=1;i<n;i++) if(a[i]<m) m=a[i]; return m; }
static int8_t se_max(const int8_t *a, int n) { int8_t m=a[0]; for(int i=1;i<n;i++) if(a[i]>m) m=a[i]; return m; }
static double se_mean(const int8_t *a, int n) { double s=0; for(int i=0;i<n;i++) s+=a[i]; return s/n; }

static void se_block(
    int8_t *feat, int C, int H, int W,
    const int8_t *w_sq, const int32_t *b_sq, int C_sq, int shift_sq,
    const int8_t *w_ex, const int32_t *b_ex, int shift_ex)
{
    int sp = H * W;
    int8_t gap[1024], sq[64], ex[1024], sc_dbg[1024];
    int fix_lut = getenv("SE_FIX_LUT") != NULL;

    fprintf(stderr, "[SE_DEBUG] feat before SE: min=%d max=%d mean=%.3f (C=%d H=%d W=%d)\n",
            se_min(feat, C*sp), se_max(feat, C*sp), se_mean(feat, C*sp), C, H, W);

    for (int c = 0; c < C; c++) {
        int32_t s = 0;
        for (int p = 0; p < sp; p++) s += feat[c*sp+p];
        s /= sp;
        gap[c] = (int8_t)(s < -128 ? -128 : s > 127 ? 127 : s);
    }
    fprintf(stderr, "[SE_DEBUG] gap: min=%d max=%d mean=%.3f\n", se_min(gap,C), se_max(gap,C), se_mean(gap,C));

    for (int co = 0; co < C_sq; co++) {
        int32_t a = b_sq ? b_sq[co] : 0;
        for (int ci = 0; ci < C; ci++) a += (int32_t)gap[ci]*w_sq[co*C+ci];
        a >>= shift_sq;
        sq[co] = (int8_t)(a < 0 ? 0 : a > 127 ? 127 : a);
    }
    fprintf(stderr, "[SE_DEBUG] sq (post-ReLU squeeze): min=%d max=%d mean=%.3f\n", se_min(sq,C_sq), se_max(sq,C_sq), se_mean(sq,C_sq));

    for (int co = 0; co < C; co++) {
        int32_t a = b_ex ? b_ex[co] : 0;
        for (int ci = 0; ci < C_sq; ci++) a += (int32_t)sq[ci]*w_ex[co*C_sq+ci];
        a >>= shift_ex;
        ex[co] = (int8_t)(a < -128 ? -128 : a > 127 ? 127 : a);
    }
    fprintf(stderr, "[SE_DEBUG] ex (pre-sigmoid excite): min=%d max=%d mean=%.3f\n", se_min(ex,C), se_max(ex,C), se_mean(ex,C));

    for (int co = 0; co < C; co++) {
        /* BUG (production code): sigmoid_lut[(uint8_t)ex[co]] -- raw
         * bit-reinterpret cast, NOT an offset. sigmoid_lut was built as
         * x=(i-128)/64 (index 128 = x=0 = sigmoid 0.5), so a correct
         * lookup needs index = ex[co]+128 (int8 -> unsigned offset),
         * not index = (uint8_t)ex[co] (bit-pattern reinterpret, which
         * swaps the two halves of the table: ex>=0 wrongly lands on
         * indices 0-127 = all-negative-x = sigmoid ~0-0.5, and ex<0
         * wrongly lands on 128-255 = all-positive-x = sigmoid ~0.5-1,
         * i.e. exactly backwards). Gated by SE_FIX_LUT env var so one
         * binary can reproduce both the bug and the fix for comparison. */
        int idx = fix_lut ? (uint8_t)((int)ex[co] + 128) : (uint8_t)ex[co];
        ex[co] = sigmoid_lut[idx];
    }
    memcpy(sc_dbg, ex, C);
    fprintf(stderr, "[SE_DEBUG] sc (post-sigmoid, fix_lut=%d): min=%d max=%d mean=%.3f\n",
            fix_lut, se_min(sc_dbg,C), se_max(sc_dbg,C), se_mean(sc_dbg,C));

    for (int c = 0; c < C; c++) {
        int16_t sc = ex[c];
        for (int p = 0; p < sp; p++) {
            int32_t v = (int32_t)feat[c*sp+p]*sc >> 7;
            feat[c*sp+p] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    }
    fprintf(stderr, "[SE_DEBUG] feat after SE: min=%d max=%d mean=%.3f\n",
            se_min(feat, C*sp), se_max(feat, C*sp), se_mean(feat, C*sp));
}

#define SWAP() do { int8_t *_t = cur; cur = nxt; nxt = _t; } while(0)

static char _ts_label[128];
#define TSTEP_FMT(...) do { \
    snprintf(_ts_label, sizeof(_ts_label), __VA_ARGS__); \
    TSTEP(_ts_label); \
} while(0)

static void repmixer_block(
    int8_t **p_cur, int8_t **p_nxt, int8_t *temp, int has_dw3,
    uintptr_t dw3_w, uintptr_t dw3_b, int shift_dw3,
    uintptr_t dw7_w, uintptr_t dw7_b, int shift_dw7,
    uintptr_t pw1_w, uintptr_t pw1_b, int shift_pw1, int C_expand,
    uintptr_t pw2_w, uintptr_t pw2_b, int shift_pw2,
    int C, int H, int W, const char *name)
{
    int8_t *cur = *p_cur, *nxt = *p_nxt;
    if (has_dw3) {
        TSTEP_FMT("  [%s] DW3 C=%d %dx%d", name, C, H, W);
        fv_run_dwconv((uintptr_t)cur,  dw3_w, dw3_b, (uintptr_t)temp,
                      C, H, W, 3, 3, 1, 1, 1, 1, 1, ACT_NONE, shift_dw3);
        TSTEP_FMT("  [%s] DW7 C=%d %dx%d", name, C, H, W);
        fv_run_dwconv((uintptr_t)temp, dw7_w, dw7_b, (uintptr_t)nxt,
                      C, H, W, 7, 7, 1, 1, 3, 3, 1, ACT_NONE, shift_dw7);
    } else {
        TSTEP_FMT("  [%s] DW7-only C=%d %dx%d", name, C, H, W);
        fv_run_dwconv((uintptr_t)cur, dw7_w, dw7_b, (uintptr_t)nxt,
                      C, H, W, 7, 7, 1, 1, 3, 3, 1, ACT_NONE, shift_dw7);
    }
    SWAP();
    TSTEP_FMT("  [%s] PW1 %d->%d %dx%d", name, C, C_expand, H, W);
    fv_run_pwconv((uintptr_t)cur, pw1_w, pw1_b, (uintptr_t)nxt,
                  C, H, W, C_expand, ACT_NONE, shift_pw1);
    SWAP();
    TSTEP_FMT("  [%s] GELU %d elem", name, C_expand * H * W);
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, C_expand, H, W);
    TSTEP_FMT("  [%s] PW2 %d->%d %dx%d", name, C_expand, C, H, W);
    fv_run_pwconv((uintptr_t)cur, pw2_w, pw2_b, (uintptr_t)temp,
                  C_expand, H, W, C, ACT_NONE, shift_pw2);
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
                3, 128, 128, 48, 2, 2, 1, 1, ACT_NONE, lw[0].out_shift); SWAP();
    TSTEP("Stem: DW3x3 48 + GELU");
    fv_run_dwconv((uintptr_t)cur, lw[1].w_addr, lw[1].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 3, 3, 1, 1, 1, 1, 1, ACT_NONE, lw[1].out_shift); SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, 48, 64, 64);
    TSTEP("Stem: PW 48->48");
    fv_run_pwconv((uintptr_t)cur, lw[2].w_addr, lw[2].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 48, ACT_NONE, lw[2].out_shift); SWAP();
    TSTEP("Stem: DW3x3 48 (no act)");
    fv_run_dwconv((uintptr_t)cur, lw[3].w_addr, lw[3].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 3, 3, 1, 1, 1, 1, 1, ACT_NONE, lw[3].out_shift); SWAP();

    /* ─── STAGE 1 (C=48, 64×64) ───────────────────────── */
    TSTEP("Stage1 blk0 RepMixer C=48 64x64");
    repmixer_block(&cur, &nxt, temp, 0,
        0, 0, 0,
        lw[4].w_addr,  lw[4].b_addr,  lw[4].out_shift,
        lw[5].w_addr,  lw[5].b_addr,  lw[5].out_shift, 144,
        lw[6].w_addr,  lw[6].b_addr,  lw[6].out_shift, 48, 64, 64,
        "S1B0");
    TSTEP("Stage1 blk1 RepMixer C=48 64x64");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[7].w_addr,  lw[7].b_addr,  lw[7].out_shift,
        lw[8].w_addr,  lw[8].b_addr,  lw[8].out_shift,
        lw[9].w_addr,  lw[9].b_addr,  lw[9].out_shift, 144,
        lw[10].w_addr, lw[10].b_addr, lw[10].out_shift, 48, 64, 64,
        "S1B1");

    /* ─── TRANSITION 1: FPGA fpg=2 (48→96, 64→32, K=7) ── */
    TSTEP("Trans1 FPGA DW7 fpg=2 48->96 64->32");
    fv_run_dwconv((uintptr_t)cur, lw[11].w_addr, lw[11].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 7, 7, 2, 2, 3, 3, 2, ACT_NONE, lw[11].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, 96, 32, 32);
    TSTEP("Trans1 PW 96->96");
    fv_run_pwconv((uintptr_t)cur, lw[12].w_addr, lw[12].b_addr, (uintptr_t)nxt,
                  96, 32, 32, 96, ACT_NONE, lw[12].out_shift); SWAP();

    /* ─── STAGE 2 (C=96, 32×32) ────────────────────────── */
    TSTEP("Stage2 blk0 RepMixer C=96 32x32");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[13].w_addr, lw[13].b_addr, lw[13].out_shift,
        lw[14].w_addr, lw[14].b_addr, lw[14].out_shift,
        lw[15].w_addr, lw[15].b_addr, lw[15].out_shift, 240,  /* PRUNED 288->240, tools/export_weights_pruned.py "top3" cut */
        lw[16].w_addr, lw[16].b_addr, lw[16].out_shift, 96, 32, 32,
        "S2B0");
    TSTEP("Stage2 blk1 RepMixer C=96 32x32");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[17].w_addr, lw[17].b_addr, lw[17].out_shift,
        lw[18].w_addr, lw[18].b_addr, lw[18].out_shift,
        lw[19].w_addr, lw[19].b_addr, lw[19].out_shift, 288,
        lw[20].w_addr, lw[20].b_addr, lw[20].out_shift, 96, 32, 32,
        "S2B1");

    /* ─── TRANSITION 2: FPGA fpg=2 (96→192, 32→16, K=7) ─ */
    TSTEP("Trans2 FPGA DW7 fpg=2 96->192 32->16");
    fv_run_dwconv((uintptr_t)cur, lw[21].w_addr, lw[21].b_addr, (uintptr_t)nxt,
                  96, 32, 32, 7, 7, 2, 2, 3, 3, 2, ACT_NONE, lw[21].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, 192, 16, 16);
    TSTEP("Trans2 PW 192->192");
    fv_run_pwconv((uintptr_t)cur, lw[22].w_addr, lw[22].b_addr, (uintptr_t)nxt,
                  192, 16, 16, 192, ACT_NONE, lw[22].out_shift); SWAP();

    /* ─── STAGE 3 (C=192, 16×16, 4 blocks) ─────────────── */
    TSTEP("Stage3 blk0 RepMixer C=192 16x16");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[23].w_addr, lw[23].b_addr, lw[23].out_shift,
        lw[24].w_addr, lw[24].b_addr, lw[24].out_shift,
        lw[25].w_addr, lw[25].b_addr, lw[25].out_shift, 576,
        lw[26].w_addr, lw[26].b_addr, lw[26].out_shift, 192, 16, 16,
        "S3B0");
    TSTEP("Stage3 blk1 RepMixer C=192 16x16");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[27].w_addr, lw[27].b_addr, lw[27].out_shift,
        lw[28].w_addr, lw[28].b_addr, lw[28].out_shift,
        lw[29].w_addr, lw[29].b_addr, lw[29].out_shift, 480,  /* PRUNED 576->480, tools/export_weights_pruned.py "top3" cut */
        lw[30].w_addr, lw[30].b_addr, lw[30].out_shift, 192, 16, 16,
        "S3B1");
    TSTEP("Stage3 blk2 RepMixer C=192 16x16");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[31].w_addr, lw[31].b_addr, lw[31].out_shift,
        lw[32].w_addr, lw[32].b_addr, lw[32].out_shift,
        lw[33].w_addr, lw[33].b_addr, lw[33].out_shift, 576,
        lw[34].w_addr, lw[34].b_addr, lw[34].out_shift, 192, 16, 16,
        "S3B2");
    TSTEP("Stage3 blk3 RepMixer C=192 16x16");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[35].w_addr, lw[35].b_addr, lw[35].out_shift,
        lw[36].w_addr, lw[36].b_addr, lw[36].out_shift,
        lw[37].w_addr, lw[37].b_addr, lw[37].out_shift, 576,
        lw[38].w_addr, lw[38].b_addr, lw[38].out_shift, 192, 16, 16,
        "S3B3");

    /* ─── TRANSITION 3: FPGA fpg=2 (192→384, 16→8, K=7) ─ */
    TSTEP("Trans3 FPGA DW7 fpg=2 192->384 16->8");
    fv_run_dwconv((uintptr_t)cur, lw[39].w_addr, lw[39].b_addr, (uintptr_t)nxt,
                  192, 16, 16, 7, 7, 2, 2, 3, 3, 2, ACT_NONE, lw[39].out_shift);
    SWAP();
    fv_run_gelu((uintptr_t)cur, (uintptr_t)cur, 384, 8, 8);
    TSTEP("Trans3 PW 384->384");
    fv_run_pwconv((uintptr_t)cur, lw[40].w_addr, lw[40].b_addr, (uintptr_t)nxt,
                  384, 8, 8, 384, ACT_NONE, lw[40].out_shift); SWAP();

    /* ─── STAGE 4 (C=384, 8×8, 2 blocks) ───────────────── */
    TSTEP("Stage4 blk0 RepMixer C=384 8x8");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[41].w_addr, lw[41].b_addr, lw[41].out_shift,
        lw[42].w_addr, lw[42].b_addr, lw[42].out_shift,
        lw[43].w_addr, lw[43].b_addr, lw[43].out_shift, 1152,
        lw[44].w_addr, lw[44].b_addr, lw[44].out_shift, 384, 8, 8,
        "S4B0");
    TSTEP("Stage4 blk1 RepMixer C=384 8x8");
    repmixer_block(&cur, &nxt, temp, 1,
        lw[45].w_addr, lw[45].b_addr, lw[45].out_shift,
        lw[46].w_addr, lw[46].b_addr, lw[46].out_shift,
        lw[47].w_addr, lw[47].b_addr, lw[47].out_shift, 960,  /* PRUNED 1152->960, tools/export_weights_pruned.py "top3" cut */
        lw[48].w_addr, lw[48].b_addr, lw[48].out_shift, 384, 8, 8,
        "S4B1");

    /* ─── FINAL DW: FPGA fpg=2 (384→768, K=3, s=2) ──────── */
    TSTEP("FinalDW FPGA DW3 fpg=2 384->768 8->4");
    fv_run_dwconv((uintptr_t)cur, lw[49].w_addr, lw[49].b_addr, (uintptr_t)nxt,
                  384, 8, 8, 3, 3, 2, 2, 1, 1, 2, ACT_NONE, lw[49].out_shift);
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
