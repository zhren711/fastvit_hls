/*============================================================
 * dwconv3_ip.cpp  v1.0
 * K=3 专用 Depthwise Conv, stride=1, pad=1 (SAME)
 * TN=4: 每 tile 处理 4 个通道, COMPUTE 4×并行
 *
 * 循环结构 (per tile):
 *   LOAD_WT:   4ch × 9wt = 36 reads  (串行, 按通道 burst)
 *   LOAD_IN:   4ch × 36px = 144 reads (串行, 每通道 6行×burst6)
 *   COMPUTE:   9×4×4 = 144 iters (PIPELINE II=1, TN unrolled)
 *   WRITE:     4ch × 16px = 64 writes
 *
 * 与 dwconv_ip(TN=1) 对比:
 *   TN=1: 241 cycles/channel → TN=4: 133 cycles/channel
 *   加速比: 1.81×
 *============================================================*/

#include "dwconv3_ip.h"

static act_t dw3_in_buf [DW3_TN][DW3_IN_TILE_H][DW3_IN_TILE_W]; /* [4][6][6] */
static wt_t  dw3_wt_buf [DW3_TN][3][3];
static acc_t dw3_out_buf[DW3_TN][DW3_TR][DW3_TC];

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

void dwconv3_ip(
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

#pragma HLS ARRAY_PARTITION variable=dw3_in_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=dw3_wt_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=dw3_out_buf dim=1 complete

    /* K=3, stride=1, pad=1: Hout=Hin, Wout=Win (SAME) */
    const int Kh = 3, Kw = 3, pad = 1;

    int Tn_loops = (CHin + DW3_TN - 1) / DW3_TN;
    int Tr_loops = (Hin  + DW3_TR - 1) / DW3_TR;
    int Tc_loops = (Win  + DW3_TC - 1) / DW3_TC;

    LOOP_DW3_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * DW3_TN;
        int ch_end   = (ch_base + DW3_TN < CHin) ? (ch_base + DW3_TN) : CHin;
        int tn_valid = ch_end - ch_base;

        /* LOAD_WT: 4ch × 3×3 = 36 reads (9/ch, 按行 burst) */
        LOAD_DW3_WT:
        for (int n = 0; n < DW3_TN; n++) {
            int ch = ch_base + n;
            for (int kh = 0; kh < Kh; kh++) {
#pragma HLS PIPELINE II=1
                for (int kw = 0; kw < Kw; kw++) {
                    dw3_wt_buf[n][kh][kw] = (n < tn_valid) ?
                        weight[ch * 9 + kh * Kw + kw] : (wt_t)0;
                }
            }
        }

        LOOP_DW3_TR:
        for (int tr = 0; tr < Tr_loops; tr++) {
            int row_out_base = tr * DW3_TR;
            int row_in_base  = row_out_base - pad;
            int tr_valid     = ((row_out_base + DW3_TR) < Hin) ?
                               DW3_TR : (Hin - row_out_base);
            int in_tile_h    = (tr_valid - 1) + Kh; /* ≤ 6 */

            LOOP_DW3_TC:
            for (int tc = 0; tc < Tc_loops; tc++) {
                int col_out_base = tc * DW3_TC;
                int col_in_base  = col_out_base - pad;
                int tc_valid     = ((col_out_base + DW3_TC) < Win) ?
                                   DW3_TC : (Win - col_out_base);
                int in_tile_w    = (tc_valid - 1) + Kw; /* ≤ 6 */

                /* LOAD_IN: 4ch × in_tile_h×in_tile_w reads */
                LOAD_DW3_IN:
                for (int n = 0; n < DW3_TN; n++) {
                    int ch = ch_base + n;
                    for (int r = 0; r < DW3_IN_TILE_H; r++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=3 max=6 avg=6
                        for (int c = 0; c < DW3_IN_TILE_W; c++) {
                            int in_r = row_in_base + r;
                            int in_c = col_in_base + c;
                            dw3_in_buf[n][r][c] =
                                (n < tn_valid && r < in_tile_h && c < in_tile_w &&
                                 in_r >= 0 && in_r < Hin && in_c >= 0 && in_c < Win) ?
                                feat_in[ch * Hin * Win + in_r * Win + in_c] : (act_t)0;
                        }
                    }
                }

                /* CLEAR → bias init */
                CLEAR_DW3_OUT:
                for (int n = 0; n < DW3_TN; n++) {
                    for (int r = 0; r < DW3_TR; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < DW3_TC; c++) {
                            int ch = ch_base + (n < tn_valid ? n : 0);
                            dw3_out_buf[n][r][c] = bias[ch];
                        }
                    }
                }

                /* COMPUTE: 3×3×TR×TC = 144 iters, TN=4 unrolled → 4 MAC/cycle */
                COMPUTE_DW3_KH:
                for (int kh = 0; kh < Kh; kh++) {
                    COMPUTE_DW3_KW:
                    for (int kw = 0; kw < Kw; kw++) {
                        COMPUTE_DW3_TR:
                        for (int r = 0; r < DW3_TR; r++) {
                            COMPUTE_DW3_TC:
                            for (int c = 0; c < DW3_TC; c++) {
#pragma HLS PIPELINE II=1
                                COMPUTE_DW3_TN:
                                for (int n = 0; n < DW3_TN; n++) {
#pragma HLS UNROLL
                                    dw3_out_buf[n][r][c] +=
                                        (acc_t)dw3_in_buf[n][r + kh][c + kw] *
                                        (acc_t)dw3_wt_buf[n][kh][kw];
                                }
                            }
                        }
                    }
                }

                /* WRITE: tn_valid ch × tr_valid × tc_valid */
                WRITE_DW3_OUT:
                for (int n = 0; n < tn_valid; n++) {
                    int ch_out_base = (ch_base + n) * Hin * Win;
                    for (int r = 0; r < tr_valid; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < tc_valid; c++) {
                            feat_out[ch_out_base + (row_out_base + r) * Win +
                                     (col_out_base + c)] =
                                apply_act(dw3_out_buf[n][r][c], act_mode, out_shift);
                        }
                    }
                }

            } /* tc */
        } /* tr */
    } /* tn */
}
