/*============================================================
 * pool_ip.h
 * 池化IP核头文件
 * 支持: MaxPool / AvgPool / GlobalAvgPool
 * 精度: int8输入输出 (GlobalAvgPool输出int32用于SE块)
 * 平台: MicroZed xc7z020clg400-1
 *============================================================*/

#ifndef __POOL_IP_H__
#define __POOL_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;
typedef ap_int<32>  acc_t;

//------------------------------------------------------------
// Tiling参数 (v3: 进一步缩小, pool_ip只用于2次GlobalAvgPool)
//------------------------------------------------------------
#define POOL_TN  1    // channel并行度 (原2 → 1)
#define POOL_TR  4    // 行tile        (原8 → 4)
#define POOL_TC  4    // 列tile        (原8 → 4)
#define MAX_POOL_K 3  // 最大pool核 (2x2, 3x3)

//------------------------------------------------------------
// 池化模式
//------------------------------------------------------------
#define POOL_MAX    0   // MaxPool
#define POOL_AVG    1   // AvgPool (整数近似)
#define POOL_GLOBAL 2   // GlobalAvgPool → int32输出 (用于SE块)

/**
 * pool_ip - 池化加速核
 *
 * @param feat_in    输入特征图 [CH * Hin * Win], int8, NCHW
 * @param feat_out   输出特征图:
 *                   - MaxPool/AvgPool: [CH * Hout * Wout], int8
 *                   - GlobalAvgPool:   [CH], int32 (cast to int8 ptr, 实际写4字节/ch)
 * @param CH         channel数
 * @param Hin        输入高度
 * @param Win        输入宽度
 * @param Kh         池化核高度
 * @param Kw         池化核宽度
 * @param stride_h   步长 (通常=Kh)
 * @param stride_w   步长 (通常=Kw)
 * @param pad_h      padding
 * @param pad_w      padding
 * @param mode       POOL_MAX / POOL_AVG / POOL_GLOBAL
 * @param out_shift  AvgPool除法右移位数 (log2(Kh*Kw))
 */
void pool_ip(
    act_t  feat_in[],
    act_t  feat_out[],    // GlobalAvgPool时输出int32需外部强转
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
    int    out_shift
);

// GlobalAvgPool专用接口 (输出int32, 直接送入FC/SE)
void global_avgpool_ip(
    act_t  feat_in[],
    acc_t  feat_out[],    // [CH], int32
    int    CH,
    int    Hin,
    int    Win
);

#endif // __POOL_IP_H__
