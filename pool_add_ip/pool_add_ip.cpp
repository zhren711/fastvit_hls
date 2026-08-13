/*============================================================
 * pool_add_ip.cpp
 * 池化 + 残差加法 合并IP核实现
 * 支持: MaxPool / AvgPool / GlobalAvgPool + 可选 ElementWise Add
 * 精度: int8 输入输出
 * 平台: MicroZed xc7z020clg400-1, Vitis HLS 2024.2
 *
 * 设计要点:
 *   1. Pool 结果直接在片上与 feat_res 相加，无中间 DDR 写入
 *   2. POOL_TN=2, POOL_TR=8, POOL_TC=8 (相比原 pool_ip 大幅缩小)
 *   3. MaxPool: 内层 TN × KH × KW 全部 UNROLL，PIPELINE II=1
 *   4. AvgPool: 累加后右移，无浮点
 *   5. GlobalAvgPool: 展平空间维度，单次扫描
 *   6. 3个 m_axi bundle，避免仲裁冲突
 *============================================================*/

#include "pool_add_ip.h"

//------------------------------------------------------------
// 片上缓冲区 (静态分配)
// in_buf: [POOL_TN][MAX_IN_H][MAX_IN_W] = [2][19][19] = 722B
// acc_buf: [POOL_TN][POOL_TR][POOL_TC]  = [2][8][8]   = 64 × 4B = 256B
//------------------------------------------------------------
static act_t in_buf [POOL_TN][MAX_IN_H][MAX_IN_W];
static acc_t acc_buf[POOL_TN][POOL_TR][POOL_TC];

//------------------------------------------------------------
// 饱和截断辅助函数
//------------------------------------------------------------
static inline act_t saturate(acc_t val) {
#pragma HLS INLINE
    if      (val >  127) return (act_t) 127;
    else if (val < -128) return (act_t)-128;
    else                 return (act_t) val;
}

//------------------------------------------------------------
// 内部: MaxPool + 可选 Add
//------------------------------------------------------------
static void maxpool_add_kernel(
    act_t feat_in[],
    act_t feat_res[],
    act_t feat_out[],
    int CH, int Hin, int Win,
    int Kh, int Kw, int sh, int sw, int ph, int pw,
    int add_en)
{
    int Hout = (Hin + 2*ph - Kh) / sh + 1;
    int Wout = (Win + 2*pw - Kw) / sw + 1;

    int Tn_loops = (CH   + POOL_TN - 1) / POOL_TN;
    int Tr_loops = (Hout + POOL_TR - 1) / POOL_TR;
    int Tc_loops = (Wout + POOL_TC - 1) / POOL_TC;

    LOOP_MP_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * POOL_TN;
        int ch_end   = (ch_base + POOL_TN < CH) ? (ch_base + POOL_TN) : CH;
        int tn_valid = ch_end - ch_base;

        LOOP_MP_TR:
        for (int tr = 0; tr < Tr_loops; tr++) {
            int r_out_base = tr * POOL_TR;
            int r_out_end  = (r_out_base + POOL_TR < Hout) ? (r_out_base + POOL_TR) : Hout;
            int tr_valid   = r_out_end - r_out_base;
            int r_in_base  = r_out_base * sh - ph;
            int in_tile_h  = (tr_valid - 1) * sh + Kh;

            LOOP_MP_TC:
            for (int tc = 0; tc < Tc_loops; tc++) {
                int c_out_base = tc * POOL_TC;
                int c_out_end  = (c_out_base + POOL_TC < Wout) ? (c_out_base + POOL_TC) : Wout;
                int tc_valid   = c_out_end - c_out_base;
                int c_in_base  = c_out_base * sw - pw;
                int in_tile_w  = (tc_valid - 1) * sw + Kw;

                // 加载输入 tile
                LOAD_MP_IN:
                for (int n = 0; n < POOL_TN; n++) {
                    for (int r = 0; r < in_tile_h; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < in_tile_w; c++) {
                            int in_r = r_in_base + r;
                            int in_c = c_in_base + c;
                            int ch   = ch_base + n;
                            if (n < tn_valid && in_r >= 0 && in_r < Hin &&
                                in_c >= 0 && in_c < Win)
                                in_buf[n][r][c] = feat_in[ch * Hin * Win + in_r * Win + in_c];
                            else
                                in_buf[n][r][c] = -128; // MaxPool 填最小值
                        }
                    }
                }

                // MaxPool 计算 + 可选 Add + 写回
                COMPUTE_MP_TR:
                for (int r = 0; r < tr_valid; r++) {
                    COMPUTE_MP_TC:
                    for (int c = 0; c < tc_valid; c++) {
#pragma HLS PIPELINE II=1
                        int out_base = (r_out_base + r) * Wout + (c_out_base + c);
                        COMPUTE_MP_TN:
                        for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
                            if (n >= tn_valid) continue;
                            act_t max_val = -128;
                            COMPUTE_MP_KH:
                            for (int kh = 0; kh < MAX_POOL_K; kh++) {
#pragma HLS UNROLL
                                if (kh >= Kh) continue;
                                COMPUTE_MP_KW:
                                for (int kw = 0; kw < MAX_POOL_K; kw++) {
#pragma HLS UNROLL
                                    if (kw >= Kw) continue;
                                    act_t val = in_buf[n][r*sh + kh][c*sw + kw];
                                    if (val > max_val) max_val = val;
                                }
                            }
                            int out_idx = (ch_base + n) * Hout * Wout + out_base;
                            if (add_en == ADD_ENABLE) {
                                wide_t sum = (wide_t)max_val + (wide_t)feat_res[out_idx];
                                feat_out[out_idx] = saturate((acc_t)sum);
                            } else {
                                feat_out[out_idx] = max_val;
                            }
                        }
                    }
                }

            } // tc
        } // tr
    } // tn
}

//------------------------------------------------------------
// 内部: AvgPool + 可选 Add
//------------------------------------------------------------
static void avgpool_add_kernel(
    act_t feat_in[],
    act_t feat_res[],
    act_t feat_out[],
    int CH, int Hin, int Win,
    int Kh, int Kw, int sh, int sw, int ph, int pw,
    int add_en, int out_shift)
{
    int Hout = (Hin + 2*ph - Kh) / sh + 1;
    int Wout = (Win + 2*pw - Kw) / sw + 1;

    int Tn_loops = (CH   + POOL_TN - 1) / POOL_TN;
    int Tr_loops = (Hout + POOL_TR - 1) / POOL_TR;
    int Tc_loops = (Wout + POOL_TC - 1) / POOL_TC;

    LOOP_AP_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * POOL_TN;
        int ch_end   = (ch_base + POOL_TN < CH) ? (ch_base + POOL_TN) : CH;
        int tn_valid = ch_end - ch_base;

        LOOP_AP_TR:
        for (int tr = 0; tr < Tr_loops; tr++) {
            int r_out_base = tr * POOL_TR;
            int r_out_end  = (r_out_base + POOL_TR < Hout) ? (r_out_base + POOL_TR) : Hout;
            int tr_valid   = r_out_end - r_out_base;
            int r_in_base  = r_out_base * sh - ph;
            int in_tile_h  = (tr_valid - 1) * sh + Kh;

            LOOP_AP_TC:
            for (int tc = 0; tc < Tc_loops; tc++) {
                int c_out_base = tc * POOL_TC;
                int c_out_end  = (c_out_base + POOL_TC < Wout) ? (c_out_base + POOL_TC) : Wout;
                int tc_valid   = c_out_end - c_out_base;
                int c_in_base  = c_out_base * sw - pw;
                int in_tile_w  = (tc_valid - 1) * sw + Kw;

                // 加载输入 tile
                LOAD_AP_IN:
                for (int n = 0; n < POOL_TN; n++) {
                    for (int r = 0; r < in_tile_h; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < in_tile_w; c++) {
                            int in_r = r_in_base + r;
                            int in_c = c_in_base + c;
                            int ch   = ch_base + n;
                            if (n < tn_valid && in_r >= 0 && in_r < Hin &&
                                in_c >= 0 && in_c < Win)
                                in_buf[n][r][c] = feat_in[ch * Hin * Win + in_r * Win + in_c];
                            else
                                in_buf[n][r][c] = 0;
                        }
                    }
                }

                // 清零累加 buffer
                CLEAR_AP:
                for (int n = 0; n < POOL_TN; n++) {
                    for (int r = 0; r < POOL_TR; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < POOL_TC; c++) {
                            acc_buf[n][r][c] = 0;
                        }
                    }
                }

                // 累加
                COMPUTE_AP_KH:
                for (int kh = 0; kh < Kh; kh++) {
                    COMPUTE_AP_KW:
                    for (int kw = 0; kw < Kw; kw++) {
                        COMPUTE_AP_TR:
                        for (int r = 0; r < tr_valid; r++) {
                            COMPUTE_AP_TC:
                            for (int c = 0; c < tc_valid; c++) {
#pragma HLS PIPELINE II=1
                                COMPUTE_AP_TN:
                                for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
                                    acc_buf[n][r][c] += (acc_t)in_buf[n][r*sh + kh][c*sw + kw];
                                }
                            }
                        }
                    }
                }

                // 右移 + 可选 Add + 写回
                WRITE_AP_OUT:
                for (int n = 0; n < tn_valid; n++) {
                    for (int r = 0; r < tr_valid; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < tc_valid; c++) {
                            act_t pool_val = saturate(acc_buf[n][r][c] >> out_shift);
                            int out_idx = (ch_base + n) * Hout * Wout +
                                          (r_out_base + r) * Wout +
                                          (c_out_base + c);
                            if (add_en == ADD_ENABLE) {
                                wide_t sum = (wide_t)pool_val + (wide_t)feat_res[out_idx];
                                feat_out[out_idx] = saturate((acc_t)sum);
                            } else {
                                feat_out[out_idx] = pool_val;
                            }
                        }
                    }
                }

            } // tc
        } // tr
    } // tn
}

//------------------------------------------------------------
// 内部: GlobalAvgPool + 可选 Add
// 输出 int8 (已做均值量化)，不同于原 global_avgpool_ip 的 int32 输出
// 这里用 out_shift = log2(Hin*Win) 做整数近似
//------------------------------------------------------------
static void global_avgpool_add_kernel(
    act_t feat_in[],
    act_t feat_res[],
    act_t feat_out[],
    int CH, int Hin, int Win,
    int add_en, int out_shift)
{
    int spatial = Hin * Win;
    int Tn_loops = (CH + POOL_TN - 1) / POOL_TN;

    static acc_t sum_buf[POOL_TN];
#pragma HLS ARRAY_PARTITION variable=sum_buf complete

    LOOP_GAP_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * POOL_TN;
        int ch_end   = (ch_base + POOL_TN < CH) ? (ch_base + POOL_TN) : CH;
        int tn_valid = ch_end - ch_base;

        // 清零
        for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
            sum_buf[n] = 0;
        }

        // 空间累加
        LOOP_GAP_H:
        for (int h = 0; h < Hin; h++) {
            LOOP_GAP_W:
            for (int w = 0; w < Win; w++) {
#pragma HLS PIPELINE II=1
                for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
                    if (n < tn_valid) {
                        sum_buf[n] += (acc_t)feat_in[(ch_base + n) * spatial + h * Win + w];
                    }
                }
            }
        }

        // 右移量化 + 可选 Add + 写回
        WRITE_GAP:
        for (int n = 0; n < tn_valid; n++) {
#pragma HLS PIPELINE II=1
            act_t pool_val = saturate(sum_buf[n] >> out_shift);
            int out_idx = ch_base + n;
            if (add_en == ADD_ENABLE) {
                wide_t sum = (wide_t)pool_val + (wide_t)feat_res[out_idx];
                feat_out[out_idx] = saturate((acc_t)sum);
            } else {
                feat_out[out_idx] = pool_val;
            }
        }
    }
}

//============================================================
// 顶层函数
//============================================================
void pool_add_ip(
    act_t  feat_in[],
    act_t  feat_res[],
    act_t  feat_out[],
    int    CH,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    pool_mode,
    int    add_en,
    int    out_shift)
{
// AXI Master 接口 (3 bundle)
#pragma HLS INTERFACE m_axi port=feat_in  offset=slave bundle=gmem0 \
    depth=65536 max_read_burst_length=256  num_read_outstanding=4
#pragma HLS INTERFACE m_axi port=feat_res offset=slave bundle=gmem1 \
    depth=65536 max_read_burst_length=256  num_read_outstanding=4
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem2 \
    depth=65536 max_write_burst_length=256 num_write_outstanding=4

// AXI Lite 控制接口
#pragma HLS INTERFACE s_axilite port=CH        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kh        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kw        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_h  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_w  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_h     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_w     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pool_mode bundle=ctrl
#pragma HLS INTERFACE s_axilite port=add_en    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return    bundle=ctrl

// 片上缓冲区分区
#pragma HLS ARRAY_PARTITION variable=in_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=acc_buf dim=1 complete

    if (pool_mode == POOL_MAX) {
        maxpool_add_kernel(feat_in, feat_res, feat_out,
                           CH, Hin, Win,
                           Kh, Kw, stride_h, stride_w, pad_h, pad_w,
                           add_en);
    } else if (pool_mode == POOL_GLOBAL) {
        global_avgpool_add_kernel(feat_in, feat_res, feat_out,
                                  CH, Hin, Win,
                                  add_en, out_shift);
    } else {
        // POOL_AVG
        avgpool_add_kernel(feat_in, feat_res, feat_out,
                           CH, Hin, Win,
                           Kh, Kw, stride_h, stride_w, pad_h, pad_w,
                           add_en, out_shift);
    }
}
