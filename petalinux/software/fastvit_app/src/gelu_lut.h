/**
 * gelu_lut.h - GELU 激活函数 int8 查找表
 * gelu(x) ≈ x * sigmoid(1.702 * x)
 * 输入/输出均为 int8, scale = 1/127
 */
#ifndef __GELU_LUT_H__
#define __GELU_LUT_H__

#include <stdint.h>

/**
 * gelu_lut[i+128] = round(gelu(i/127.0) * 127)
 * 索引: i ∈ [-128, 127], 访问时用 gelu_lut[x + 128]
 */
extern const int8_t gelu_lut[256];

/** 
 * 对 feature map 做 in-place GELU
 * @param data  int8 数组
 * @param size  元素总数
 */
static inline void apply_gelu(int8_t *data, int size)
{
    for (int i = 0; i < size; i++) {
        data[i] = gelu_lut[(int)data[i] + 128];
    }
}

#endif /* __GELU_LUT_H__ */
