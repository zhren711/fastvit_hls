/*============================================================
 * pool_ip.cpp  v2 — GlobalAvgPool only (FastVIT SE块专用)
 *
 * FastVIT 中 pool_ip 仅用于 2 次 GlobalAvgPool (SE块ReduceMean)，
 * 不使用 MaxPool/AvgPool。去掉空间 tile buffer，改为逐像素累加，
 * 大幅节省 LUT/FF。
 *
 * 接口与 v1 相同（mode/Kh/Kw/stride/pad 参数保留但忽略），
 * ARM 驱动端不需要改动。
 *============================================================*/

#include "pool_ip.h"

// 量化 + 裁剪
static inline act_t quant_clip(acc_t v, int shift) {
#pragma HLS INLINE
    acc_t s = v >> shift;
    if      (s >  127) return  127;
    else if (s < -128) return -128;
    else               return (act_t)s;
}

//============================================================
// 顶层函数（仅实现 GlobalAvgPool，其余 mode 参数保留接口）
//============================================================
void pool_ip(
    act_t  feat_in[],
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
    int    mode,
    int    out_shift)
{
#pragma HLS INTERFACE m_axi port=feat_in  offset=slave bundle=gmem0 depth=65536
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem1 depth=65536
#pragma HLS INTERFACE s_axilite port=CH       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kh       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kw       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_h bundle=ctrl
#pragma HLS INTERFACE s_axilite port=stride_w bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_h    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_w    bundle=ctrl
#pragma HLS INTERFACE s_axilite port=mode     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return   bundle=ctrl

    int spatial  = Hin * Win;
    int Tn_loops = (CH + POOL_TN - 1) / POOL_TN;

    // 每次处理 POOL_TN 个 channel（=1）
    LOOP_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * POOL_TN;
        int ch_end   = (ch_base + POOL_TN < CH) ? (ch_base + POOL_TN) : CH;
        int tn_valid = ch_end - ch_base;

        // 累加器（POOL_TN=1, 极小）
        acc_t sum[POOL_TN];
#pragma HLS ARRAY_PARTITION variable=sum complete

        INIT_SUM:
        for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
            sum[n] = 0;
        }

        // 逐像素累加（行优先）
        int ch0 = ch_base; // POOL_TN=1，只有一个 channel
        LOOP_H:
        for (int h = 0; h < Hin; h++) {
            int row_base = ch0 * spatial + h * Win;
            LOOP_W:
            for (int w = 0; w < Win; w++) {
#pragma HLS PIPELINE II=1
                sum[0] += (acc_t)feat_in[row_base + w];
            }
        }

        // 量化输出
        WRITE_OUT:
        for (int n = 0; n < tn_valid; n++) {
#pragma HLS PIPELINE II=1
            feat_out[ch_base + n] = quant_clip(sum[n], out_shift);
        }
    }
}

//============================================================
// GlobalAvgPool 专用接口（int32 输出，兼容旧驱动）
//============================================================
void global_avgpool_ip(
    act_t  feat_in[],
    acc_t  feat_out[],
    int    CH,
    int    Hin,
    int    Win)
{
#pragma HLS INTERFACE m_axi port=feat_in  offset=slave bundle=gmem0 depth=65536
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem1 depth=512
#pragma HLS INTERFACE s_axilite port=CH   bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl

    int spatial  = Hin * Win;
    int Tn_loops = (CH + POOL_TN - 1) / POOL_TN;

    static acc_t sum_buf[POOL_TN];
#pragma HLS ARRAY_PARTITION variable=sum_buf complete

    LOOP_GAP_TN:
    for (int tn = 0; tn < Tn_loops; tn++) {
        int ch_base  = tn * POOL_TN;
        int ch_end   = (ch_base + POOL_TN < CH) ? (ch_base + POOL_TN) : CH;
        int tn_valid = ch_end - ch_base;

        CLEAR_GAP:
        for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
            sum_buf[n] = 0;
        }

        LOOP_GAP_H:
        for (int h = 0; h < Hin; h++) {
            LOOP_GAP_W:
            for (int w = 0; w < Win; w++) {
#pragma HLS PIPELINE II=1
                LOOP_GAP_N:
                for (int n = 0; n < POOL_TN; n++) {
#pragma HLS UNROLL
                    if (n < tn_valid) {
                        sum_buf[n] += (acc_t)feat_in[(ch_base + n) * spatial + h * Win + w];
                    }
                }
            }
        }

        WRITE_GAP:
        for (int n = 0; n < tn_valid; n++) {
#pragma HLS PIPELINE II=1
            feat_out[ch_base + n] = sum_buf[n];
        }
    }
}
