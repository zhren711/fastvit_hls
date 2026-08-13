/*============================================================
 * conv_ip.cpp  v2 — Standard 3×3 Conv ONLY
 * 量化: int8 激活 + int8 权重 → int32 累加 → int8 输出
 * 平台: MicroZed xc7z020clg400-1, Vitis HLS 2024.2
 *
 * 设计决策:
 *   1. 只保留 3×3 Standard Conv (DW→dwconv_ip, PW→pwconv_ip)
 *   2. MAX_K 固定=3，消除 K 维度的大量 MUX
 *   3. in_buf/wt_buf/out_buf 全部 dim=1 complete partition
 *   4. 最内层 MAC 循环 PIPELINE II=1，TM×TN UNROLL
 *   5. stride 仅支持 1/2，运行时 if 分支，不引入乘法器
 *============================================================*/

#include "conv_ip.h"

//------------------------------------------------------------
// 片上缓冲区 (静态，避免栈溢出)
//------------------------------------------------------------
static act_t in_buf [TN][IN_TILE_H][IN_TILE_W];  // 2×6×6
static wt_t  wt_buf [TM][TN][CONV_K][CONV_K];    // 2×2×3×3
static acc_t out_buf[TM][TR][TC];                  // 2×4×4

//------------------------------------------------------------
// 量化激活: 右移 + 饱和 + ReLU (可选)
//------------------------------------------------------------
static inline act_t quant_act(acc_t v, int shift, int relu) {
#pragma HLS INLINE
    acc_t s = v >> shift;
    act_t r;
    if      (s >  127) r =  127;
    else if (s < -128) r = -128;
    else               r = (act_t)s;
    if (relu && r < 0) r = 0;
    return r;
}

//============================================================
// 顶层函数
//============================================================
void conv_ip(
    act_t  feat_in[],
    wt_t   weight[],
    acc_t  bias[],
    act_t  feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    CHout,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    act_mode,
    int    out_shift)
{
// ── AXI 接口 ────────────────────────────────────────────────
#pragma HLS INTERFACE m_axi port=feat_in  offset=slave bundle=gmem0 depth=65536
#pragma HLS INTERFACE m_axi port=weight   offset=slave bundle=gmem1 depth=65536
#pragma HLS INTERFACE m_axi port=bias     offset=slave bundle=gmem2 depth=512
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem3 depth=65536
#pragma HLS INTERFACE s_axilite port=CHin      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=CHout     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_h  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_w  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_h     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_w     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=act_mode  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return    bundle=ctrl

// ── 缓冲区划分 ──────────────────────────────────────────────
#pragma HLS ARRAY_PARTITION variable=in_buf  dim=1 complete   // TN 维
#pragma HLS ARRAY_PARTITION variable=wt_buf  dim=1 complete   // TM 维
#pragma HLS ARRAY_PARTITION variable=wt_buf  dim=2 complete   // TN 维
#pragma HLS ARRAY_PARTITION variable=out_buf dim=1 complete   // TM 维

    // 输出尺寸
    int Hout = (Hin + 2*pad_h - CONV_K) / stride_h + 1;
    int Wout = (Win + 2*pad_w - CONV_K) / stride_w + 1;

    // Tile 循环数
    int Tm_loops = (CHout + TM - 1) / TM;
    int Tn_loops = (CHin  + TN - 1) / TN;
    int Tr_loops = (Hout  + TR - 1) / TR;
    int Tc_loops = (Wout  + TC - 1) / TC;

    int relu = (act_mode == 1) ? 1 : 0;

    //----------------------------------------------------------
    // 外层循环: 输出 channel tile × 空间 tile
    //----------------------------------------------------------
    LOOP_TM:
    for (int tm = 0; tm < Tm_loops; tm++) {
        int cout_base = tm * TM;
        int cout_end  = (cout_base + TM < CHout) ? cout_base + TM : CHout;
        int tm_valid  = cout_end - cout_base;

        LOOP_TR:
        for (int tr = 0; tr < Tr_loops; tr++) {
            int row_out_base = tr * TR;
            int row_out_end  = (row_out_base + TR < Hout) ? row_out_base + TR : Hout;
            int tr_valid     = row_out_end - row_out_base;
            int row_in_base  = row_out_base * stride_h - pad_h;
            // 输入 tile 高度 (stride=1: tr_valid-1+K; stride=2: (tr_valid-1)*2+K)
            int in_tile_h    = (tr_valid - 1) * stride_h + CONV_K;

            LOOP_TC:
            for (int tc = 0; tc < Tc_loops; tc++) {
                int col_out_base = tc * TC;
                int col_out_end  = (col_out_base + TC < Wout) ? col_out_base + TC : Wout;
                int tc_valid     = col_out_end - col_out_base;
                int col_in_base  = col_out_base * stride_w - pad_w;
                int in_tile_w    = (tc_valid - 1) * stride_w + CONV_K;

                // 初始化输出 buffer (加 bias)
                INIT_OUT:
                for (int m = 0; m < TM; m++) {
                    int ch_out = (m < tm_valid) ? (cout_base + m) : cout_base;
                    for (int r = 0; r < TR; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < TC; c++) {
                            out_buf[m][r][c] = bias[ch_out];
                        }
                    }
                }

                //----------------------------------------------
                // 输入 channel tile
                //----------------------------------------------
                LOOP_TN:
                for (int tn = 0; tn < Tn_loops; tn++) {
                    int cin_base = tn * TN;
                    int cin_end  = (cin_base + TN < CHin) ? cin_base + TN : CHin;
                    int tn_valid = cin_end - cin_base;

                    // 加载输入 tile [TN][in_tile_h][in_tile_w]
                    // ── 200MHz 时序优化: 预计算行基地址，消除 PIPELINE 内乘法 ──
                    LOAD_IN:
                    for (int n = 0; n < TN; n++) {
                        int ch = cin_base + n;
                        int ch_base_addr = ch * Hin * Win;   // 提到 r 循环外
                        for (int r = 0; r < IN_TILE_H; r++) {
                            int in_r = row_in_base + r;
                            // in_r*Win: 提到 c PIPELINE 外，消除关键路径乘法
                            int row_base = ch_base_addr + in_r * Win;
                            bool row_valid = (n < tn_valid) && (in_r >= 0) && (in_r < Hin) && (r < in_tile_h);
#pragma HLS PIPELINE II=1
                            for (int c = 0; c < IN_TILE_W; c++) {
                                int in_c = col_in_base + c;   // 只含加法，快
                                if (row_valid && in_c >= 0 && in_c < Win && c < in_tile_w)
                                    in_buf[n][r][c] = feat_in[row_base + in_c];
                                else
                                    in_buf[n][r][c] = 0;
                            }
                        }
                    }

                    // 加载权重 tile [TM][TN][3][3]
                    LOAD_WT:
                    for (int m = 0; m < TM; m++) {
                        for (int n = 0; n < TN; n++) {
                            for (int kh = 0; kh < CONV_K; kh++) {
#pragma HLS PIPELINE II=1
                                for (int kw = 0; kw < CONV_K; kw++) {
                                    int cout_idx = cout_base + m;
                                    int cin_idx  = cin_base  + n;
                                    if (m < tm_valid && n < tn_valid)
                                        wt_buf[m][n][kh][kw] = weight[
                                            cout_idx * CHin * CONV_K * CONV_K +
                                            cin_idx  * CONV_K * CONV_K +
                                            kh * CONV_K + kw];
                                    else
                                        wt_buf[m][n][kh][kw] = 0;
                                }
                            }
                        }
                    }

                    // MAC: 200MHz 优化 — ir/ic 提到 TC PIPELINE 外，消除关键路径乘法
                    COMPUTE_KH:
                    for (int kh = 0; kh < CONV_K; kh++) {
                        COMPUTE_KW:
                        for (int kw = 0; kw < CONV_K; kw++) {
                            COMPUTE_TR:
                            for (int r = 0; r < TR; r++) {
                                // 行索引: 提到 TC PIPELINE 外
                                int ir = (stride_h == 1) ? (r + kh) : (r + r + kh);
                                COMPUTE_TC:
                                for (int c = 0; c < TC; c++) {
#pragma HLS PIPELINE II=1
                                    // 列索引: 用 MUX 代替乘法
                                    int ic = (stride_w == 1) ? (c + kw) : (c + c + kw);
                                    COMPUTE_TM:
                                    for (int m = 0; m < TM; m++) {
#pragma HLS UNROLL
                                        acc_t sum = 0;
                                        COMPUTE_TN:
                                        for (int n = 0; n < TN; n++) {
#pragma HLS UNROLL
                                            sum += (acc_t)in_buf[n][ir][ic] * (acc_t)wt_buf[m][n][kh][kw];
                                        }
                                        out_buf[m][r][c] += sum;
                                    }
                                }
                            }
                        }
                    }
                } // tn

                // 写回输出 (200MHz: 预计算行基地址，消除 PIPELINE 内乘法)
                WRITE_OUT:
                for (int m = 0; m < TM; m++) {
                    if (m >= tm_valid) break;
                    int ch_out = cout_base + m;
                    int ch_base_out = ch_out * Hout * Wout;  // 提到最外层
                    for (int r = 0; r < TR; r++) {
                        if (r >= tr_valid) break;
                        int row_base_out = ch_base_out + (row_out_base + r) * Wout;  // 提到 c PIPELINE 外
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < TC; c++) {
                            if (c >= tc_valid) break;
                            int idx = row_base_out + (col_out_base + c);  // 只含加法
                            feat_out[idx] = quant_act(out_buf[m][r][c], out_shift, relu);
                        }
                    }
                }

            } // tc
        } // tr
    } // tm
}
