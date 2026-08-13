/*============================================================
 * add_ip.h
 * 残差加法 IP 核头文件 (Element-wise Add)
 * 用于 FastVIT 的 89 个残差连接
 * 精度: int8输入输出 (带饱和裁剪)
 * 平台: MicroZed xc7z020clg400-1
 *============================================================*/

#ifndef __ADD_IP_H__
#define __ADD_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;     // 激活值 int8

//------------------------------------------------------------
// Tiling参数
//------------------------------------------------------------
#define ADD_TN  4    // channel并行度 (8→4, 省~750 LUT)
#define ADD_TR  8    // 行tile (16→8)
#define ADD_TC  8    // 列tile (16→8)

/**
 * add_ip - 元素级加法核 (残差连接)
 *
 * @param feat_in1   输入特征图1 [CH * H * W], int8, NCHW
 * @param feat_in2   输入特征图2 [CH * H * W], int8, NCHW (残差分支)
 * @param feat_out   输出特征图 [CH * H * W], int8, 带饱和裁剪
 * @param CH         channel数
 * @param H          高度
 * @param W          宽度
 *
 * 功能: feat_out = clip(feat_in1 + feat_in2, -128, 127)
 */
void add_ip(
    act_t  feat_in1[],
    act_t  feat_in2[],
    act_t  feat_out[],
    int    CH,
    int    H,
    int    W
);

#endif // __ADD_IP_H__
