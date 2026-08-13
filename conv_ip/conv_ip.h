/*============================================================
 * conv_ip.h  v2 — Standard Conv ONLY (3x3, int8)
 * 精简方案:
 *   - 去掉 DW/PW 模式 (已有独立 dwconv_ip / pwconv_ip)
 *   - MAX_K=3 (FastVIT stem/standard layers 全部 3x3)
 *   - 小 tiling: TN=2, TM=2, TR=4, TC=4
 *   - 分离 stride=1 / stride=2 路径，消除运行时乘法
 * 目标资源: LUT < 20K, FF < 25K, DSP < 60
 * 平台: MicroZed xc7z020clg400-1
 *============================================================*/

#ifndef __CONV_IP_H__
#define __CONV_IP_H__

#include <ap_int.h>

//------------------------------------------------------------
// 数据类型
//------------------------------------------------------------
typedef ap_int<8>   act_t;
typedef ap_int<8>   wt_t;
typedef ap_int<32>  acc_t;

//------------------------------------------------------------
// Tiling 参数 — 保守设置，优先保证资源不超标
//   TN=2, TM=2: 每次处理 2个输入channel / 2个输出channel
//   TR=4, TC=4: 4×4 输出空间 tile
//   MAX_K=3:    只支持 3×3 卷积核
//
// 理论资源估算 (TN=2, TM=2, TR=4, TC=4):
//   in_buf  [2][6][6]  = 72  bytes → 2 份 LUTRAM
//   wt_buf  [2][2][3][3] = 36 bytes → LUTRAM
//   out_buf [2][4][4]  = 32 × int32 → 128 FF
//   MAC 并行度: TM×TN = 4 DSP
//   预估: ~12K LUT, ~15K FF, ~40 DSP
//------------------------------------------------------------
#define TN      2    // TN=2, TM=2 (200MHz + 资源重新充足)
#define TM      2
#define TR      4
#define TC      4

#define CONV_K  3               // 固定 3×3 卷积核

// in_buf tile 尺寸 — 按 stride=2 最坏情况分配
// stride=2: in_tile = (T-1)*2 + K = (4-1)*2+3 = 9
// stride=1: in_tile = (T-1)*1 + K = (4-1)+3   = 6
// 统一用 9，多余空间不影响正确性
#define IN_TILE_H  ((TR-1)*2 + CONV_K)  // = 9
#define IN_TILE_W  ((TC-1)*2 + CONV_K)  // = 9

#define MAX_CH  512

//------------------------------------------------------------
// 顶层函数接口
// 注: stride_h/stride_w 仍保留为参数（1 或 2），
//     但计算中通过常量分支优化展开
//------------------------------------------------------------
void conv_ip(
    act_t  feat_in[],   // [CHin * Hin * Win]
    wt_t   weight[],    // [CHout * CHin * 3 * 3]
    acc_t  bias[],      // [CHout]
    act_t  feat_out[],  // [CHout * Hout * Wout]
    int    CHin,
    int    Hin,
    int    Win,
    int    CHout,
    int    stride_h,    // 1 or 2
    int    stride_w,    // 1 or 2
    int    pad_h,       // typically 1
    int    pad_w,
    int    act_mode,    // 0=none, 1=relu
    int    out_shift    // 量化右移位数
);

#endif // __CONV_IP_H__
