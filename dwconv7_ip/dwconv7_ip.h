/*============================================================
 * dwconv7_ip.h  v1.0
 * Depthwise Convolution IP — K=7 专用, TN=2 并行
 *
 * 固定参数: K=7, stride=1, pad=3 (SAME)
 * 并行度:   DW7_TN=2 → COMPUTE 2× 加速 (compute-bound)
 *
 * 性能 vs dwconv_ip(TN=1):
 *   K=7 per-channel cycles: 949 → 557 (~1.7× 加速)
 *   原因: COMPUTE(84%) 2×加速, LOAD 略增
 *
 * 资源估算:
 *   dw7_in_buf [2][10][10] = 200B (vs [1][10][10]=100B)
 *   dw7_wt_buf [2][7][7]   = 98B  (vs [1][7][7]=49B)
 *   dw7_out_buf[2][4][4]   = 32B
 *   DSP: 2 (vs 1)
 *   Slice: +100~150 vs dwconv_ip TN=1
 *============================================================*/

#ifndef __DWCONV7_IP_H__
#define __DWCONV7_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;

#define DW7_TN    2    /* channel 并行度 */
#define DW7_TR    4    /* 输出行 tile */
#define DW7_TC    4    /* 输出列 tile */

/* K=7, stride=1, pad=3 → 输入 tile = (TR+K-1) × (TC+K-1) = 10×10 */
#define DW7_IN_TILE_H  10
#define DW7_IN_TILE_W  10

#define ACT_NONE  0
#define ACT_RELU  1

/**
 * dwconv7_ip — K=7 Depthwise Convolution (stride=1, SAME padding)
 */
void dwconv7_ip(
    act_t  feat_in[],   /* [CHin × Hin × Win], int8, NCHW */
    wt_t   weight[],    /* [CHin × 7 × 7], int8            */
    acc_t  bias[],      /* [CHin], int32                    */
    act_t  feat_out[],  /* [CHin × Hin × Win], int8, NCHW  */
    int    CHin,
    int    Hin,
    int    Win,
    int    act_mode,
    int    out_shift
);

#endif /* __DWCONV7_IP_H__ */
