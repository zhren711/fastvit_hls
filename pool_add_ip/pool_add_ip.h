/*============================================================
 * pool_add_ip.h
 * 池化 + 残差加法 合并IP核头文件
 * 功能: Pool → 可选 ElementWise Add，一次完成，省去中间 DDR round-trip
 *
 * 相比分离的 pool_ip + add_ip:
 *   - 减少 2 个 m_axi 接口 (节省 ~1,500 LUT)
 *   - 省去 pool 结果写回 DDR 再读入 add 的开销
 *   - POOL_TN/TR/TC 缩小，大幅降低 in_buf / unroll 资源
 *
 * 精度: int8 输入输出
 * 平台: MicroZed xc7z020clg400-1, Vitis HLS 2024.2
 *============================================================*/

#ifndef __POOL_ADD_IP_H__
#define __POOL_ADD_IP_H__

#include <ap_int.h>

typedef ap_int<8>   act_t;
typedef ap_int<32>  acc_t;
typedef ap_int<9>   wide_t;   // 用于 add 饱和中间值

//------------------------------------------------------------
// Tiling 参数 (v3: 进一步缩小，节省 LUT)
// v2: POOL_TN=2, POOL_TR=8,  POOL_TC=8  → 39,468 LUT (74%)
// v3: POOL_TN=1, POOL_TR=4,  POOL_TC=4  → 预计 ~20K LUT
// in_buf 从 [2][19][19]=722B → [1][11][11]=121B, 缩 ~6x
//------------------------------------------------------------
#define POOL_TN      1
#define POOL_TR      4
#define POOL_TC      4
#define MAX_POOL_K   3    // 最大池化核 (2x2, 3x3)

//------------------------------------------------------------
// Pool 模式
//------------------------------------------------------------
#define POOL_MAX    0
#define POOL_AVG    1
#define POOL_GLOBAL 2   // Global AvgPool (Kh=Hin, Kw=Win)

//------------------------------------------------------------
// Add 使能
//------------------------------------------------------------
#define ADD_DISABLE 0
#define ADD_ENABLE  1

//------------------------------------------------------------
// in_buf 最大尺寸
// MAX_IN_H = (POOL_TR-1)*max_stride + MAX_POOL_K = 3*2+3 = 9
// 留 2 的余量 → 11
//------------------------------------------------------------
#define MAX_IN_H  (POOL_TR * 2 + MAX_POOL_K)   // = 11
#define MAX_IN_W  (POOL_TC * 2 + MAX_POOL_K)   // = 11

/**
 * pool_add_ip - 池化 + 可选残差加法合并核
 *
 * @param feat_in   输入特征图 [CH * Hin * Win], int8, NCHW (gmem0, read)
 * @param feat_res  残差输入   [CH * Hout * Wout], int8 (gmem1, read)
 *                  add_en=ADD_DISABLE 时此指针不访问
 * @param feat_out  输出       [CH * Hout * Wout], int8 (gmem2, write)
 * @param CH        channel 数
 * @param Hin       输入高度
 * @param Win       输入宽度
 * @param Kh        池化核高度 (1~MAX_POOL_K)
 * @param Kw        池化核宽度
 * @param stride_h  步长
 * @param stride_w  步长
 * @param pad_h     padding
 * @param pad_w     padding
 * @param pool_mode POOL_MAX / POOL_AVG / POOL_GLOBAL
 * @param add_en    ADD_ENABLE: pool结果 + feat_res; ADD_DISABLE: 只pool
 * @param out_shift AvgPool 累加结果右移位数 = log2(Kh*Kw)
 */
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
    int    out_shift
);

#endif // __POOL_ADD_IP_H__
