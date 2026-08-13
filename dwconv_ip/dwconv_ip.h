/*============================================================
 * dwconv_ip.h
 * Depthwise Convolution IP 核头文件
 * 用于 FastVIT 中所有 DW Conv 层（group=CHin=CHout）
 * 精度: int8 激活 + int8 权重 → int32 累加 → int8 输出
 * 平台: MicroZed xc7z020clg400-1, Vitis HLS 2024.2
 *
 * 支持: K=3(stride=1/2), K=7(stride=1/2, SAME/VALID padding)
 * 支持: fpg>=1 (expand factor: 每个输入 channel 输出 fpg 个 channel)
 *============================================================*/

#ifndef __DWCONV_IP_H__
#define __DWCONV_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;

#define DW_TN    1    /* channel 并行度 */
#define DW_TR    4
#define DW_TC    4
#define DW_MAX_K 7
#define DW_MAX_CH 512
#define DW_MAX_H  64  /* max feature map height (Stage1) */
#define DW_MAX_W  64  /* max feature map width */

/* stride=2 requires (DW_TR-1)*2 + DW_MAX_K = 13 rows in input tile */
#define DW_MAX_IN_TILE_H  ((DW_TR - 1) * 2 + DW_MAX_K)
#define DW_MAX_IN_TILE_W  ((DW_TC - 1) * 2 + DW_MAX_K)

#define ACT_NONE  0
#define ACT_RELU  1

void dwconv_ip(
    ap_uint<32> feat_in[],   /* 32-bit packed: 4 bytes per AXI beat */
    wt_t        weight[],
    acc_t       bias[],
    ap_uint<32> feat_out[],  /* 32-bit packed: 4 bytes per AXI beat */
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    fpg,       /* expand factor: CHout = CHin * fpg */
    int    act_mode,
    int    out_shift
);

#endif // __DWCONV_IP_H__
