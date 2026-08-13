/*============================================================
 * add_ip.cpp
 * 残差加法 IP 核实现 (Element-wise Add)
 * 用于 FastVIT 的 89 个残差连接
 * 精度: int8输入输出 (带饱和裁剪)
 * 平台: MicroZed xc7z020clg400-1, Vitis HLS 2024.2
 *
 * 优化要点:
 *   1. 去掉 tile 循环，改为平坦化线性遍历 → 保证 II=1 pipeline
 *   2. 使用 ap_int<9> 中间结果（只需要9bit存储-256~255）
 *   3. burst 读写：AXI4 Master 自动合并为 burst 传输
 *   4. 双端口 gmem，读写并发
 *============================================================*/

#include "add_ip.h"

void add_ip(
    act_t  feat_in1[],
    act_t  feat_in2[],
    act_t  feat_out[],
    int    CH,
    int    H,
    int    W)
{
#pragma HLS INTERFACE m_axi port=feat_in1 offset=slave bundle=gmem0 \
    depth=65536 max_read_burst_length=256 num_read_outstanding=4
#pragma HLS INTERFACE m_axi port=feat_in2 offset=slave bundle=gmem1 \
    depth=65536 max_read_burst_length=256 num_read_outstanding=4
#pragma HLS INTERFACE m_axi port=feat_out offset=slave bundle=gmem2 \
    depth=65536 max_write_burst_length=256 num_write_outstanding=4
#pragma HLS INTERFACE s_axilite port=CH     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=H      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=W      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl

    int total = CH * H * W;

    ADD_LOOP:
    for (int i = 0; i < total; i++) {
#pragma HLS PIPELINE II=1
        // ap_int<9> 防止 int8+int8 溢出（范围 -256~255）
        ap_int<9> sum = (ap_int<9>)feat_in1[i] + (ap_int<9>)feat_in2[i];
        act_t result;
        if      (sum >  127) result =  127;
        else if (sum < -128) result = -128;
        else                 result = (act_t)sum;
        feat_out[i] = result;
    }
}
