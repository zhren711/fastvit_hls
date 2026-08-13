/**
 * fastvit_infer.c
 * FastVIT-T8 @ 128x128  推理调度器 (int8量化)
 * 模型: fastvit_t8_processed_128x128.onnx (52 Conv 层)
 *
 * 硬件划分:
 *   FPGA: conv_ip / dwconv_ip / pwconv_ip / add_ip
 *   ARM:  GELU(LUT) / SE块 / 3个Transition扩展DWConv / Final扩展DW / Sigmoid / Mul
 *
 * 层索引 (对应 weights_t8/quant_config.json 中 0..51):
 *   lw[0]  : Stem Conv(3→48, K=3, s=2)
 *   lw[1]  : Stem DW(48, K=3) + GELU
 *   lw[2]  : Stem PW(48→48)
 *   lw[3]  : Stem DW(48, K=3) [no GELU]
 *   lw[4]  : Stage1 blk0 DW(48, K=7) [token mixer, 仅 DW7，无前置 DW3]
 *   lw[5]  : Stage1 blk0 PW expand(48→144)
 *   lw[6]  : Stage1 blk0 PW compress(144→48)
 *   lw[7]  : Stage1 blk1 DW(48, K=3) [前置 DW3]
 *   lw[8]  : Stage1 blk1 DW(48, K=7) [token mixer]
 *   lw[9]  : Stage1 blk1 PW expand(48→144)
 *   lw[10] : Stage1 blk1 PW compress(144→48)
 *   lw[11] : Trans1 DW expand(48→96, K=7, s=2) [ARM]
 *   lw[12] : Trans1 PW(96→96)
 *   lw[13] : Stage2 blk0 DW(96, K=3)
 *   lw[14] : Stage2 blk0 DW(96, K=7) [token mixer]
 *   lw[15] : Stage2 blk0 PW expand(96→288)
 *   lw[16] : Stage2 blk0 PW compress(288→96)
 *   lw[17] : Stage2 blk1 DW(96, K=3)
 *   lw[18] : Stage2 blk1 DW(96, K=7)
 *   lw[19] : Stage2 blk1 PW expand(96→288)
 *   lw[20] : Stage2 blk1 PW compress(288→96)
 *   lw[21] : Trans2 DW expand(96→192, K=7, s=2) [ARM]
 *   lw[22] : Trans2 PW(192→192)
 *   lw[23..26] : Stage3 blk0: DW3,DW7,PW expand(192→576),PW compress(576→192)
 *   lw[27..30] : Stage3 blk1: DW3,DW7,PW,PW
 *   lw[31..34] : Stage3 blk2: DW3,DW7,PW,PW
 *   lw[35..38] : Stage3 blk3: DW3,DW7,PW,PW
 *   lw[39] : Trans3 DW expand(192→384, K=7, s=2) [ARM]
 *   lw[40] : Trans3 PW(384→384)
 *   lw[41..44] : Stage4 blk0: DW3(384),DW7(384),PW(384→1152),PW(1152→384)
 *   lw[45..48] : Stage4 blk1: DW3,DW7,PW,PW
 *   lw[49] : Final DW expand(384→768, K=3, s=2) [ARM]
 *   lw[50] : SE squeeze PW(768→48)
 *   lw[51] : SE excite  PW(48→768)
 *
 * Token Mixer 结构 (blk0 of Stage1 vs 其他所有块):
 *   blk0 Stage1: DW7(x) → token_mix  (DW7 已含残差)
 *   其他所有块:  DW3(x) → DW7(DW3(x)) → token_mix
 *   MLP: PW_expand(token_mix) → GELU → PW_compress → Add(token_mix, compress)
 *
 * 输出: int8 feature [768 x 4 x 4]
 *
 * 缓冲区 (DDR):
 *   FV_FEAT_PING_BASE: ping (~2MB)
 *   FV_FEAT_PONG_BASE: pong (~2MB)
 *   FV_FEAT_TEMP_BASE: temp (DW3输出, PW compress输出, ~512KB)
 */

#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "gelu_lut.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

/* ── Sigmoid LUT ─────────────────────────────────────────── */
static int8_t sigmoid_lut[256];
static int sigmoid_lut_ready = 0;

static void init_sigmoid_lut(void)
{
    if (sigmoid_lut_ready) return;
    for (int i = 0; i < 256; i++) {
        float xf = (i - 128) * (1.0f / 64.0f);
        float s  = 1.0f / (1.0f + expf(-xf));
        sigmoid_lut[i] = (int8_t)(s * 127.0f + 0.5f);
    }
    sigmoid_lut_ready = 1;
}

/* ── Transition DWConv (ARM): group=Cin, Cout>Cin ─────────── */
static void transition_dwconv(
    const int8_t  *in, int8_t *out,
    const int8_t  *wt, const int32_t *bias,
    int Cin, int Hin, int Win,
    int Cout, int Kh, int Kw, int stride, int pad, int shift)
{
    int Hout = (Hin + 2*pad - Kh) / stride + 1;
    int Wout = (Win + 2*pad - Kw) / stride + 1;
    int fpg  = Cout / Cin;
    for (int co = 0; co < Cout; co++) {
        int ci = co / fpg;
        for (int oh = 0; oh < Hout; oh++) {
            for (int ow = 0; ow < Wout; ow++) {
                int32_t acc = bias ? bias[co] : 0;
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ih = oh*stride - pad + kh;
                        int iw = ow*stride - pad + kw;
                        if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                            acc += (int32_t)in[ci*Hin*Win + ih*Win + iw]
                                 * wt[co*(fpg*Kh*Kw) + (co%fpg)*Kh*Kw + kh*Kw + kw];
                    }
                }
                acc >>= shift;
                out[co*Hout*Wout + oh*Wout + ow] =
                    (int8_t)(acc < -128 ? -128 : acc > 127 ? 127 : acc);
            }
        }
    }
}

/* ── SE 块 (ARM) ──────────────────────────────────────────── */
static void se_block(
    int8_t *feat, int C, int H, int W,
    const int8_t *w_sq, const int32_t *b_sq, int C_sq, int shift_sq,
    const int8_t *w_ex, const int32_t *b_ex, int shift_ex)
{
    int sp = H * W;
    int8_t gap[512], sq[64], ex[512];
    for (int c = 0; c < C; c++) {
        int32_t s = 0;
        for (int p = 0; p < sp; p++) s += feat[c*sp + p];
        s /= sp;
        gap[c] = (int8_t)(s < -128 ? -128 : s > 127 ? 127 : s);
    }
    for (int co = 0; co < C_sq; co++) {
        int32_t acc = b_sq ? b_sq[co] : 0;
        for (int ci = 0; ci < C; ci++) acc += (int32_t)gap[ci] * w_sq[co*C + ci];
        acc >>= shift_sq;
        sq[co] = (int8_t)(acc < 0 ? 0 : acc > 127 ? 127 : acc);
    }
    for (int co = 0; co < C; co++) {
        int32_t acc = b_ex ? b_ex[co] : 0;
        for (int ci = 0; ci < C_sq; ci++) acc += (int32_t)sq[ci] * w_ex[co*C_sq + ci];
        acc >>= shift_ex;
        ex[co] = (int8_t)(acc < -128 ? -128 : acc > 127 ? 127 : acc);
    }
    for (int co = 0; co < C; co++) ex[co] = sigmoid_lut[(uint8_t)ex[co]];
    for (int c = 0; c < C; c++) {
        int16_t sc = ex[c];
        for (int p = 0; p < sp; p++) {
            int32_t v = (int32_t)feat[c*sp + p] * sc >> 7;
            feat[c*sp + p] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * RepMixer Block (正确的 T8 token mixer):
 *   has_dw3=0: token_mix = DW7(x)         [Stage1 blk0]
 *   has_dw3=1: token_mix = DW7(DW3(x))    [所有其他块]
 *   MLP: PW_expand → GELU → PW_compress → Add(token_mix, compress)
 * ═══════════════════════════════════════════════════════════ */
#define SWAP() do { int8_t *_t = cur; cur = nxt; nxt = _t; } while(0)

static void repmixer_block(
    int8_t **p_cur, int8_t **p_nxt,
    int8_t *temp,
    int has_dw3,
    uintptr_t dw3_w, uintptr_t dw3_b, int shift_dw3,
    uintptr_t dw7_w, uintptr_t dw7_b, int shift_dw7,
    uintptr_t pw1_w, uintptr_t pw1_b, int shift_pw1, int C_expand,
    uintptr_t pw2_w, uintptr_t pw2_b, int shift_pw2,
    int C, int H, int W)
{
    int8_t *cur = *p_cur;
    int8_t *nxt = *p_nxt;

    /* ── Token Mixer ── */
    if (has_dw3) {
        fv_run_dwconv((uintptr_t)cur,  dw3_w, dw3_b, (uintptr_t)temp,
                      C, H, W, 3, 3, 1, 1, 1, 1, ACT_NONE, shift_dw3);
        fv_run_dwconv((uintptr_t)temp, dw7_w, dw7_b, (uintptr_t)nxt,
                      C, H, W, 7, 7, 1, 1, 3, 3, ACT_NONE, shift_dw7);
    } else {
        fv_run_dwconv((uintptr_t)cur, dw7_w, dw7_b, (uintptr_t)nxt,
                      C, H, W, 7, 7, 1, 1, 3, 3, ACT_NONE, shift_dw7);
    }
    SWAP();
    /* cur = token_mix, nxt = old x */

    /* ── MLP: expand → GELU → compress → Add ── */
    fv_run_pwconv((uintptr_t)cur, pw1_w, pw1_b, (uintptr_t)nxt,
                  C, H, W, C_expand, ACT_NONE, shift_pw1);
    SWAP();
    /* cur = expanded, nxt = token_mix */

    apply_gelu(cur, C_expand * H * W);

    fv_run_pwconv((uintptr_t)cur, pw2_w, pw2_b, (uintptr_t)temp,
                  C_expand, H, W, C, ACT_NONE, shift_pw2);
    /* temp = compress */

    fv_run_add((uintptr_t)nxt, (uintptr_t)temp, (uintptr_t)cur, C, H, W);
    /* cur = token_mix + compress = block output */

    *p_cur = cur;
    *p_nxt = nxt;
}

/* ═══════════════════════════════════════════════════════════
 * fastvit_t8_infer()
 *   input:  int8 [3 x 128 x 128]
 *   output: int8 [768 x 4 x 4]
 *   lw:     52 层权重 (0..51)
 * ═══════════════════════════════════════════════════════════ */
int fastvit_t8_infer(
    const int8_t      *input,
    int8_t            *output,
    const LayerWeight *lw,
    int8_t            *ping,
    int8_t            *pong)
{
    init_sigmoid_lut();

    int8_t *cur  = ping;
    int8_t *nxt  = pong;
    int8_t *temp = (int8_t *)FV_FEAT_TEMP_BASE;

    memcpy(cur, input, 3 * 128 * 128);

    /* ─── STEM ─────────────────────────────────────────── */
    fv_run_conv((uintptr_t)cur, lw[0].w_addr, lw[0].b_addr, (uintptr_t)nxt,
                3, 128, 128, 48, 2, 2, 1, 1, ACT_NONE, lw[0].out_shift);
    SWAP();

    fv_run_dwconv((uintptr_t)cur, lw[1].w_addr, lw[1].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 3, 3, 1, 1, 1, 1, ACT_NONE, lw[1].out_shift);
    SWAP();
    apply_gelu(cur, 48 * 64 * 64);

    fv_run_pwconv((uintptr_t)cur, lw[2].w_addr, lw[2].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 48, ACT_NONE, lw[2].out_shift);
    SWAP();

    fv_run_dwconv((uintptr_t)cur, lw[3].w_addr, lw[3].b_addr, (uintptr_t)nxt,
                  48, 64, 64, 3, 3, 1, 1, 1, 1, ACT_NONE, lw[3].out_shift);
    SWAP();

    /* ─── STAGE 1 (C=48, 64×64) ───────────────────────── */
    repmixer_block(&cur, &nxt, temp, 0,
        0, 0, 0,
        lw[4].w_addr, lw[4].b_addr, lw[4].out_shift,
        lw[5].w_addr, lw[5].b_addr, lw[5].out_shift, 144,
        lw[6].w_addr, lw[6].b_addr, lw[6].out_shift,
        48, 64, 64);

    repmixer_block(&cur, &nxt, temp, 1,
        lw[7].w_addr, lw[7].b_addr, lw[7].out_shift,
        lw[8].w_addr, lw[8].b_addr, lw[8].out_shift,
        lw[9].w_addr, lw[9].b_addr, lw[9].out_shift, 144,
        lw[10].w_addr, lw[10].b_addr, lw[10].out_shift,
        48, 64, 64);

    /* ─── TRANSITION 1 (ARM: 48→96, 64→32, K=7, s=2) ─── */
    transition_dwconv(cur, nxt,
        (const int8_t *)lw[11].w_addr, (const int32_t *)lw[11].b_addr,
        48, 64, 64, 96, 7, 7, 2, 3, lw[11].out_shift);
    SWAP();
    apply_gelu(cur, 96 * 32 * 32);

    fv_run_pwconv((uintptr_t)cur, lw[12].w_addr, lw[12].b_addr, (uintptr_t)nxt,
                  96, 32, 32, 96, ACT_NONE, lw[12].out_shift);
    SWAP();

    /* ─── STAGE 2 (C=96, 32×32, 2 blocks) ─────────────── */
    repmixer_block(&cur, &nxt, temp, 1,
        lw[13].w_addr, lw[13].b_addr, lw[13].out_shift,
        lw[14].w_addr, lw[14].b_addr, lw[14].out_shift,
        lw[15].w_addr, lw[15].b_addr, lw[15].out_shift, 288,
        lw[16].w_addr, lw[16].b_addr, lw[16].out_shift,
        96, 32, 32);

    repmixer_block(&cur, &nxt, temp, 1,
        lw[17].w_addr, lw[17].b_addr, lw[17].out_shift,
        lw[18].w_addr, lw[18].b_addr, lw[18].out_shift,
        lw[19].w_addr, lw[19].b_addr, lw[19].out_shift, 288,
        lw[20].w_addr, lw[20].b_addr, lw[20].out_shift,
        96, 32, 32);

    /* ─── TRANSITION 2 (ARM: 96→192, 32→16) ────────────── */
    transition_dwconv(cur, nxt,
        (const int8_t *)lw[21].w_addr, (const int32_t *)lw[21].b_addr,
        96, 32, 32, 192, 7, 7, 2, 3, lw[21].out_shift);
    SWAP();
    apply_gelu(cur, 192 * 16 * 16);

    fv_run_pwconv((uintptr_t)cur, lw[22].w_addr, lw[22].b_addr, (uintptr_t)nxt,
                  192, 16, 16, 192, ACT_NONE, lw[22].out_shift);
    SWAP();

    /* ─── STAGE 3 (C=192, 16×16, 4 blocks) ─────────────── */
    for (int blk = 0; blk < 4; blk++) {
        int base = 23 + blk * 4;
        repmixer_block(&cur, &nxt, temp, 1,
            lw[base+0].w_addr, lw[base+0].b_addr, lw[base+0].out_shift,
            lw[base+1].w_addr, lw[base+1].b_addr, lw[base+1].out_shift,
            lw[base+2].w_addr, lw[base+2].b_addr, lw[base+2].out_shift, 576,
            lw[base+3].w_addr, lw[base+3].b_addr, lw[base+3].out_shift,
            192, 16, 16);
    }

    /* ─── TRANSITION 3 (ARM: 192→384, 16→8) ────────────── */
    transition_dwconv(cur, nxt,
        (const int8_t *)lw[39].w_addr, (const int32_t *)lw[39].b_addr,
        192, 16, 16, 384, 7, 7, 2, 3, lw[39].out_shift);
    SWAP();
    apply_gelu(cur, 384 * 8 * 8);

    fv_run_pwconv((uintptr_t)cur, lw[40].w_addr, lw[40].b_addr, (uintptr_t)nxt,
                  384, 8, 8, 384, ACT_NONE, lw[40].out_shift);
    SWAP();

    /* ─── STAGE 4 (C=384, 8×8, 2 blocks) ───────────────── */
    repmixer_block(&cur, &nxt, temp, 1,
        lw[41].w_addr, lw[41].b_addr, lw[41].out_shift,
        lw[42].w_addr, lw[42].b_addr, lw[42].out_shift,
        lw[43].w_addr, lw[43].b_addr, lw[43].out_shift, 1152,
        lw[44].w_addr, lw[44].b_addr, lw[44].out_shift,
        384, 8, 8);

    repmixer_block(&cur, &nxt, temp, 1,
        lw[45].w_addr, lw[45].b_addr, lw[45].out_shift,
        lw[46].w_addr, lw[46].b_addr, lw[46].out_shift,
        lw[47].w_addr, lw[47].b_addr, lw[47].out_shift, 1152,
        lw[48].w_addr, lw[48].b_addr, lw[48].out_shift,
        384, 8, 8);

    /* ─── FINAL: DW expand (ARM: 384→768, K=3, s=2) ────── */
    transition_dwconv(cur, nxt,
        (const int8_t *)lw[49].w_addr, (const int32_t *)lw[49].b_addr,
        384, 8, 8, 768, 3, 3, 2, 1, lw[49].out_shift);
    SWAP();

    /* ─── SE @ 768ch, 4×4 (ARM) ─────────────────────────── */
    se_block(cur, 768, 4, 4,
        (const int8_t  *)lw[50].w_addr,
        (const int32_t *)lw[50].b_addr, 48, lw[50].out_shift,
        (const int8_t  *)lw[51].w_addr,
        (const int32_t *)lw[51].b_addr, lw[51].out_shift);

    memcpy(output, cur, 768 * 4 * 4 * sizeof(int8_t));
    return 0;
}
