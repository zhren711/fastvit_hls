/*============================================================
 * dwconv7_ip.cpp  v1.0
 * K=7 专用 Depthwise Conv, stride=1, pad=3 (SAME)
 * TN=2: 每 tile 处理 2 个通道, COMPUTE 2×并行
 *
 * K=7 bottleneck 分析:
 *   COMPUTE: 7×7×4×4 = 784 iters, 占总 cycle 的 ~84%
 *   LOAD_IN: 10×10 = 100 reads/ch, 占 ~11%
 *   TN=2 将 COMPUTE 2×加速, 总体 1.70×
 *
 * 循环结构 (per tile, 2 channels):
 *   LOAD_WT:   2ch × 49wt = 98 reads
 *   LOAD_IN:   2ch × 100px = 200 reads
 *   COMPUTE:   49×4×4 = 784 iters (PIPELINE II=1, TN=2 unrolled)
 *   WRITE:     2ch × 16px = 32 writes
 *   Total:     1114 cycles / 2ch → 557 cycles/ch (vs 949)
 *============================================================*/

#include "dwconv7_ip.h"

static act_t dw7_in_buf [DW7_TN][DW7_IN_TILE_H][DW7_IN_TILE_W]; /* [2][10][10] */
static wt_t  dw7_wt_buf [DW7_TN][7][7];
static acc_t dw7_out_buf[DW7_TN][DW7_TR][DW7_TC];

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

void dwconv7_ip(
    act_t  feat_in[],
    wt_t   weight[],
    acc_t  bias[],
    act_t  feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    act_mode,
    int    out_shift)
{
#pragma HLS INTERFACE m_axi port=feat_in  offset=slave bundle=gmem0 depth=65536 \
    max_read_burst_length=16 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=weight   offset=slave bundle=gmem1 depth=4096  \
    max_read_burst_length=16 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=bias     offset=slave bundle=gmem2 depth=512
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem3 depth=65536 \
    max_write_burst_length=16 num_write_outstanding=16

#pragma HLS INTERFACE s_axilite port=CHin      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=act_mode  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return    bundle=ctrl

#pragma HLS ARRAY_PARTITION variable=dw7_in_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=dw7_wt_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=dw7_out_buf dim=1 complete

    /* K=7, stride=1, pad=3: Hout=Hin, Wout=Win (SAME) */
    const int Kh = 7, Kw = 7, pad = 3;

    int Tn_loops = (CHin + DW7_TN - 1) / DW7_TN;
    int Tr_loops = (Hin  + DW7_TR - 1) / DW7_TR;
    int Tc_loops = (Win  + DW7_TC - 1) / DW7_TC;

    LOOP_DW7_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * DW7_TN;
        int ch_end   = (ch_base + DW7_TN < CHin) ? (ch_base + DW7_TN) : CHin;
        int tn_valid = ch_end - ch_base;

        /* LOAD_WT: 2ch × 7×7 = 98 reads (49/ch, 按行 burst-7) */
        LOAD_DW7_WT:
        for (int n = 0; n < DW7_TN; n++) {
            int ch = ch_base + n;
            for (int kh = 0; kh < Kh; kh++) {
#pragma HLS PIPELINE II=1
                for (int kw = 0; kw < Kw; kw++) {
                    dw7_wt_buf[n][kh][kw] = (n < tn_valid) ?
                        weight[ch * 49 + kh * Kw + kw] : (wt_t)0;
                }
            }
        }

        LOOP_DW7_TR:
        for (int tr = 0; tr < Tr_loops; tr++) {
            int row_out_base = tr * DW7_TR;
            int row_in_base  = row_out_base - pad;
            int tr_valid     = ((row_out_base + DW7_TR) < Hin) ?
                               DW7_TR : (Hin - row_out_base);
            int in_tile_h    = (tr_valid - 1) + Kh; /* ≤ 10 */

            LOOP_DW7_TC:
            for (int tc = 0; tc < Tc_loops; tc++) {
                int col_out_base = tc * DW7_TC;
                int col_in_base  = col_out_base - pad;
                int tc_valid     = ((col_out_base + DW7_TC) < Win) ?
                                   DW7_TC : (Win - col_out_base);
                int in_tile_w    = (tc_valid - 1) + Kw; /* ≤ 10 */

                /* LOAD_IN: 2ch × 10×10 reads (每行 burst-10) */
                LOAD_DW7_IN:
                for (int n = 0; n < DW7_TN; n++) {
                    int ch = ch_base + n;
                    for (int r = 0; r < DW7_IN_TILE_H; r++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=7 max=10 avg=10
                        for (int c = 0; c < DW7_IN_TILE_W; c++) {
                            int in_r = row_in_base + r;
                            int in_c = col_in_base + c;
                            dw7_in_buf[n][r][c] =
                                (n < tn_valid && r < in_tile_h && c < in_tile_w &&
                                 in_r >= 0 && in_r < Hin && in_c >= 0 && in_c < Win) ?
                                feat_in[ch * Hin * Win + in_r * Win + in_c] : (act_t)0;
                        }
                    }
                }

                /* CLEAR → bias init */
                CLEAR_DW7_OUT:
                for (int n = 0; n < DW7_TN; n++) {
                    for (int r = 0; r < DW7_TR; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < DW7_TC; c++) {
                            int ch = ch_base + (n < tn_valid ? n : 0);
                            dw7_out_buf[n][r][c] = bias[ch];
                        }
                    }
                }

                /* COMPUTE: 7×7×TR×TC = 784 iters, TN=2 unrolled → 2 MAC/cycle */
                COMPUTE_DW7_KH:
                for (int kh = 0; kh < Kh; kh++) {
                    COMPUTE_DW7_KW:
                    for (int kw = 0; kw < Kw; kw++) {
                        COMPUTE_DW7_TR:
                        for (int r = 0; r < DW7_TR; r++) {
                            COMPUTE_DW7_TC:
                            for (int c = 0; c < DW7_TC; c++) {
#pragma HLS PIPELINE II=1
                                COMPUTE_DW7_TN:
                                for (int n = 0; n < DW7_TN; n++) {
#pragma HLS UNROLL
                                    dw7_out_buf[n][r][c] +=
                                        (acc_t)dw7_in_buf[n][r + kh][c + kw] *
                                        (acc_t)dw7_wt_buf[n][kh][kw];
                                }
                            }
                        }
                    }
                }

                /* WRITE: tn_valid ch × tr_valid × tc_valid */
                WRITE_DW7_OUT:
                for (int n = 0; n < tn_valid; n++) {
                    int ch_out_base = (ch_base + n) * Hin * Win;
                    for (int r = 0; r < tr_valid; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < tc_valid; c++) {
                            feat_out[ch_out_base + (row_out_base + r) * Win +
                                     (col_out_base + c)] =
                                apply_act(dw7_out_buf[n][r][c], act_mode, out_shift);
                        }
                    }
                }

            } /* tc */
        } /* tr */
    } /* tn */
}
