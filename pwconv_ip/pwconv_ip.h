/*============================================================
 * pwconv_ip.h  v14 (final optimal for xc7z020)
 *
 * 硬件极限分析:
 *   xc7z020 Slice 预算不允许:
 *   - TM>8 (TM=16: +704 Slices, TM=32: OOC 崩溃)
 *   - 同时缓存 feat_in+weight (Slice 超限)
 *
 *   任意循环顺序的总 LOAD 工作量不变:
 *   (Tm,Tn,Ts): feat_in 冗余=Tm_loops, weight 冗余=1
 *   (Ts,Tn,Tm): feat_in 冗余=1, weight 冗余=Ts_loops
 *   总量相似 → 循环顺序改变无本质提升
 *
 * 当前最优配置 (TM=8, TN=4, TS=8, 32-bit AXI):
 *   - pwconv: ~420ms
 *   - 全局 PL: ~777ms
 *   - 已达 xc7z020 资源极限
 *============================================================*/

#ifndef __PWCONV_IP_H__
#define __PWCONV_IP_H__

#include <ap_int.h>

typedef ap_int<8>    act_t;
typedef ap_int<8>    wt_t;
typedef ap_int<32>   acc_t;
typedef ap_uint<32>  data32_t;

#define PW_TM    8    /* 输出 channel 并行度 */
#define PW_TN    4    /* 输入 channel 并行度 */
#define PW_TS    8    /* 空间 tile */

#define ACT_NONE  0
#define ACT_RELU  1

void pwconv_ip(
    data32_t feat_in_w32[],
    wt_t     weight[],
    acc_t    bias[],
    data32_t feat_out_w32[],
    int      CHin,
    int      H,
    int      W,
    int      CHout,
    int      act_mode,
    int      out_shift
);

#endif // __PWCONV_IP_H__
