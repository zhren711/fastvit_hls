/*============================================================
 * dwconv3_ip.h  v1.0
 * Depthwise Convolution IP — K=3 专用, TN=4 并行
 *
 * 固定参数: K=3, stride=1, pad=1 (SAME)
 * 并行度:   DW3_TN=4 → COMPUTE 4× 加速
 *
 * 性能 vs dwconv_ip(TN=1):
 *   K=3 per-channel cycles: 241 → 133 (~1.8× 加速)
 *   原因: COMPUTE 4×加速, LOAD_IN 不变(顺序访问 per 通道)
 *
 * 资源估算:
 *   dw3_in_buf [4][6][6]  = 144B  (vs [1][10][10]=100B)
 *   dw3_wt_buf [4][3][3]  = 36B
 *   dw3_out_buf[4][4][4]  = 64B
 *   DSP: 4 (vs 1)
 *   Slice: +100~200 vs dwconv_ip TN=1
 *============================================================*/

#ifndef __DWCONV3_IP_H__
#define __DWCONV3_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;

#define DW3_TN    4    /* channel 并行度 */
#define DW3_TR    4    /* 输出行 tile */
#define DW3_TC    4    /* 输出列 tile */

/* K=3, stride=1, pad=1 → 输入 tile = (TR+K-1) × (TC+K-1) = 6×6 */
#define DW3_IN_TILE_H  6
#define DW3_IN_TILE_W  6

#define ACT_NONE  0
#define ACT_RELU  1

/**
 * dwconv3_ip — K=3 Depthwise Convolution (stride=1, SAME padding)
 * 接口去掉 K/stride/pad 参数 (硬编码), 节省 AXI-Lite 寄存器
 */
void dwconv3_ip(
    act_t  feat_in[],   /* [CHin × Hin × Win], int8, NCHW */
    wt_t   weight[],    /* [CHin × 3 × 3], int8            */
    acc_t  bias[],      /* [CHin], int32                    */
    act_t  feat_out[],  /* [CHin × Hin × Win], int8, NCHW  */
    int    CHin,
    int    Hin,
    int    Win,
    int    act_mode,
    int    out_shift
);

#endif /* __DWCONV3_IP_H__ */
