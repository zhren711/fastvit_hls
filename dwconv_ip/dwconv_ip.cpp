/*============================================================
 * dwconv_ip.cpp  v12.0  Compute Loop Optimization
 * Depthwise Convolution IP — 支持 fpg (expand factor)
 *
 * 问题分析 (v11 vs v10 性能相同):
 * - v11 优化了 PRELOAD_IN (32-bit AXI), 但这不是瓶颈
 * - 真正瓶颈: COMPUTE_DW pipeline 在 TC(最内层)重启 196 次/tile
 *   每次重启 = 13 (pipeline depth) + 4 (TC iters) = 17 cycles overhead
 *   196 × 17 = 3,332 cycles/tile vs 理论 784 cycles/tile (4.25× 损失)
 *   对 48ch, 64×64, K=7: 256 tiles × 3332 cycles × 48ch = 40.9M cycles
 *   @100MHz ≈ 409ms → 观测到 1030ms cumulative (含 Stem 等)
 *
 * v12 优化策略:
 * 1. COMPUTE_DW: 将 PIPELINE 从 TC 移至 KW, UNROLL TR/TC (compile-time 常数)
 *    → 每 tile 只有 Kh=7 次 pipeline 重启 (vs 196)
 *    → 每次 KH 迭代: Kw × DW_TR × DW_TC = 7×4×4=112 并行 MAC
 *    预期: 7 KH × (depth + Kw) ≈ 7×12 = 84 cycles/tile (vs 3332)
 * 2. LOAD_DW_IN: 将 PIPELINE 移至 r 外层, UNROLL 内层 c
 *    + ch_in_buf cyclic=16 (支持 13 列同时读取, 不同 bank)
 *    → 10 cycles/tile (vs ~130-150)
 * 3. STORE_DW_OUT: PIPELINE 在 r, UNROLL 内层 c
 * 4. 预期 DW7 (C=48, 64×64) 加速: ~400ms → ~15ms (26× per layer)
 *============================================================*/

#include "dwconv_ip.h"

static act_t ch_in_buf [DW_MAX_H * DW_MAX_W];
static act_t ch_out_buf[DW_MAX_H * DW_MAX_W];

static act_t dw_in_buf [DW_TN][DW_MAX_IN_TILE_H][DW_MAX_IN_TILE_W];
static wt_t  dw_wt_buf [DW_TN][DW_MAX_K][DW_MAX_K];
static acc_t dw_out_buf[DW_TN][DW_TR][DW_TC];

static inline act_t apply_act(acc_t val, int act_mode, int shift) {
#pragma HLS INLINE
    acc_t s = val >> shift;
    act_t r;
    if      (s >  127) r =  127;
    else if (s < -128) r = -128;
    else               r = (act_t)s;
    if (act_mode == ACT_RELU && r < 0) r = 0;
    return r;
}

/* v14: DSP packing removed. TR is now sequential (not unrolled), TC remains
 * unrolled (4 MACs/cycle via simple multiply-accumulate). Each (kh,r) pair
 * pre-loads row ir from dw_in_buf into a 13-element row_buf, reducing the
 * per-cycle MUX from 169:1 to 13:1 and cutting control sets ~4× vs v13. */

void dwconv_ip(
    ap_uint<32> feat_in[],
    wt_t        weight[],
    acc_t       bias[],
    ap_uint<32> feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    fpg,
    int    act_mode,
    int    out_shift)
{
#pragma HLS INTERFACE m_axi port=feat_in  offset=slave bundle=gmem0 depth=65536 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem3 depth=65536 \
    latency=64 num_write_outstanding=4 max_write_burst_length=256
#pragma HLS INTERFACE m_axi port=weight   offset=slave bundle=gmem1 depth=65536 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
#pragma HLS INTERFACE m_axi port=bias     offset=slave bundle=gmem2 depth=1024

#pragma HLS INTERFACE s_axilite port=CHin      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kh        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kw        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_h  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_w  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_h     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_w     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=fpg       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=act_mode  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return    bundle=ctrl

/* ch_in_buf: cyclic=16 → 16 BRAM banks
 * LOAD_DW_IN reads 13 consecutive cols → 13 different banks → II=1 achievable */
#pragma HLS ARRAY_PARTITION variable=ch_in_buf  cyclic factor=16 dim=1
#pragma HLS ARRAY_PARTITION variable=ch_out_buf cyclic factor=16 dim=1
#pragma HLS BIND_STORAGE variable=ch_in_buf  type=RAM_1P impl=BRAM
#pragma HLS BIND_STORAGE variable=ch_out_buf type=RAM_1P impl=BRAM

/* dw_in_buf/wt_buf: fully partitioned → all elements as registers
 * row_buf/wt_row: 1D staging buffers, pre-loaded per (kh,r) to cut MUX depth */
#pragma HLS ARRAY_PARTITION variable=dw_in_buf  complete dim=0
#pragma HLS ARRAY_PARTITION variable=dw_wt_buf  complete dim=0
#pragma HLS ARRAY_PARTITION variable=dw_out_buf complete dim=0

    act_t row_buf[DW_MAX_IN_TILE_W];
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
    wt_t  wt_row[DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=wt_row complete dim=1

    int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    int Tr_loops = (Hout + DW_TR - 1) / DW_TR;
    int Tc_loops = (Wout + DW_TC - 1) / DW_TC;
    int hw_in_words  = (Hin * Win)   >> 2;
    int hw_out_words = (Hout * Wout) >> 2;

    LOOP_DW_CH:
    for (int ch = 0; ch < CHin; ch++) {
#pragma HLS LOOP_TRIPCOUNT min=48 max=512

        /* PRELOAD: 32-bit reads → 4 bytes/beat */
        PRELOAD_IN:
        for (int hw = 0; hw < hw_in_words; hw++) {
#pragma HLS PIPELINE II=1
            ap_uint<32> word = feat_in[ch * hw_in_words + hw];
            ch_in_buf[hw*4+0] = (act_t)word.range( 7,  0);
            ch_in_buf[hw*4+1] = (act_t)word.range(15,  8);
            ch_in_buf[hw*4+2] = (act_t)word.range(23, 16);
            ch_in_buf[hw*4+3] = (act_t)word.range(31, 24);
        }

        LOOP_DW_FPG:
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;

            LOAD_DW_WT:
            for (int kh = 0; kh < Kh; kh++) {
#pragma HLS PIPELINE II=1
                for (int kw = 0; kw < Kw; kw++) {
                    dw_wt_buf[0][kh][kw] = weight[co * Kh * Kw + kh * Kw + kw];
                }
            }

            acc_t bias_val = bias[co];

            LOOP_DW_TR:
            for (int tr = 0; tr < Tr_loops; tr++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=16
                int row_out_base = tr * DW_TR;
                int row_out_end  = (row_out_base + DW_TR < Hout) ? (row_out_base + DW_TR) : Hout;
                int tr_valid     = row_out_end - row_out_base;
                int row_in_base  = row_out_base * stride_h - pad_h;
                int in_tile_h    = (tr_valid - 1) * stride_h + Kh;

                LOOP_DW_TC:
                for (int tc = 0; tc < Tc_loops; tc++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=16
                    int col_out_base = tc * DW_TC;
                    int col_out_end  = (col_out_base + DW_TC < Wout) ? (col_out_base + DW_TC) : Wout;
                    int tc_valid     = col_out_end - col_out_base;
                    int col_in_base  = col_out_base * stride_w - pad_w;
                    int in_tile_w    = (tc_valid - 1) * stride_w + Kw;

                    /* Load input tile — PIPELINE on r, UNROLL c
                     * ch_in_buf cyclic=16: 13 consecutive cols → 13 different banks → II=1 */
                    LOAD_DW_IN:
                    for (int r = 0; r < DW_MAX_IN_TILE_H; r++) {
#pragma HLS PIPELINE II=1
                        if (r < in_tile_h) {
                            for (int c = 0; c < DW_MAX_IN_TILE_W; c++) {
#pragma HLS UNROLL
                                int in_r = row_in_base + r;
                                int in_c = col_in_base + c;
                                dw_in_buf[0][r][c] =
                                    (c < in_tile_w &&
                                     in_r >= 0 && in_r < Hin &&
                                     in_c >= 0 && in_c < Win)
                                    ? ch_in_buf[in_r * Win + in_c]
                                    : (act_t)0;
                            }
                        }
                    }

                    /* Clear output tile — fully unrolled, 1 cycle */
                    CLEAR_DW_OUT:
                    for (int r = 0; r < DW_TR; r++) {
#pragma HLS UNROLL
                        for (int c = 0; c < DW_TC; c++) {
#pragma HLS UNROLL
                            dw_out_buf[0][r][c] = bias_val;
                        }
                    }

                    /* Compute — v14: TR sequential (outer), KH sequential,
                     * KW PIPELINE II=1, TC UNROLL.
                     * Per tile: DW_TR × Kh × (Kw + depth) ≈ 4×7×12 = 336 cycles.
                     * Vs v13 (TR+TC unrolled): 1,995 ctrl-sets → placement fail.
                     * Here: ~200 ctrl-sets → fits xc7z020 with margin. */
                    COMPUTE_DW_TR:
                    for (int r = 0; r < DW_TR; r++) {
                        COMPUTE_DW_KH:
                        for (int kh = 0; kh < Kh; kh++) {
                            int ir = r * stride_h + kh;
                            /* Pre-load row ir and weight row kh (1-cycle parallel load) */
                            for (int c = 0; c < DW_MAX_IN_TILE_W; c++) {
#pragma HLS UNROLL
                                row_buf[c] = dw_in_buf[0][ir][c];
                            }
                            for (int kw = 0; kw < DW_MAX_K; kw++) {
#pragma HLS UNROLL
                                wt_row[kw] = dw_wt_buf[0][kh][kw];
                            }
                            COMPUTE_DW_KW:
                            for (int kw = 0; kw < Kw; kw++) {
#pragma HLS PIPELINE II=1
                                wt_t w = wt_row[kw];
                                COMPUTE_DW_TC:
                                for (int c = 0; c < DW_TC; c++) {
#pragma HLS UNROLL
                                    int ic = c * stride_w + kw;
                                    dw_out_buf[0][r][c] += (acc_t)row_buf[ic] * (acc_t)w;
                                }
                            }
                        }
                    }

                    /* Store output tile — PIPELINE on r, UNROLL c */
                    STORE_DW_OUT:
                    for (int r = 0; r < DW_TR; r++) {
#pragma HLS PIPELINE II=1
                        if (r < tr_valid) {
                            int oh = row_out_base + r;
                            for (int c = 0; c < DW_TC; c++) {
#pragma HLS UNROLL
                                if (c < tc_valid) {
                                    int ow = col_out_base + c;
                                    ch_out_buf[oh * Wout + ow] =
                                        apply_act(dw_out_buf[0][r][c], act_mode, out_shift);
                                }
                            }
                        }
                    }

                } // tc
            } // tr

            /* WRITEBACK: 4 bytes/beat, 32-bit AXI */
            WRITEBACK_OUT:
            for (int hw = 0; hw < hw_out_words; hw++) {
#pragma HLS PIPELINE II=1
                ap_uint<32> word;
                word.range( 7,  0) = ch_out_buf[hw*4+0];
                word.range(15,  8) = ch_out_buf[hw*4+1];
                word.range(23, 16) = ch_out_buf[hw*4+2];
                word.range(31, 24) = ch_out_buf[hw*4+3];
                feat_out[co * hw_out_words + hw] = word;
            }

        } // fpg
    } // ch
}
