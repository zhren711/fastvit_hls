/**
 * fastvit_infer.h
 * FastVIT-T8 @ 128x128 推理调度器
 * 管理 DDR 内存布局、层序列调度、GELU/SE/Transition 软件处理
 *
 * 模型: FastVIT-T8 (reparameterize + int8 量化)
 * 输入: [3 x 128 x 128] int8
 * 输出: [768 x 4 x 4]   int8 (backbone feature, 无分类头)
 */
#ifndef __FASTVIT_INFER_H__
#define __FASTVIT_INFER_H__

#include <stdint.h>
#include "fastvit_driver.h"

/* ── DDR 内存布局 (256MB 以上区域，避开 Linux 内核) ─────────
 * 裸机模式下可自由分配，建议放在 0x1000_0000 以上
 * 总需求估算:
 *   最大 feature map: Stage1 [64*128*128] = 1MB
 *   权重: 约 32MB
 *   双缓冲 ping-pong: 2 * 1MB = 2MB
 */
#define FV_DDR_BASE         0x10000000UL   // 权重起始地址
#define FV_FEAT_PING_BASE   0x12100000UL   // Ping feature buffer (~2MB each)
#define FV_FEAT_PONG_BASE   0x12300000UL
#define FV_FEAT_TEMP_BASE   0x12500000UL   // 临时 buffer (残差分支用)

/* ── 层权重地址表（fastvit_infer.c 使用）────────────────── */
typedef struct {
    uint32_t  w_addr;     /* 权重在 DDR 的物理地址 (int8) */
    uint32_t  b_addr;     /* 偏置在 DDR 的物理地址 (int32) */
    uint32_t  w_size;     /* 权重字节数 */
    uint32_t  b_size;     /* 偏置字节数 */
    int32_t   out_shift;  /* 量化右移位数 */
} LayerWeight;

/* ── (旧版，兼容保留) ───────────────────────────────────── */
typedef struct {
    int     layer_idx;
    char    op_type[16];
    int     kernel[2];
    int     group;
    int     CHout, CHin;
    float   input_scale;
    float  *weight_scale;
    float   output_scale;
    int     out_shift;
    uintptr_t weight_addr;
    uintptr_t bias_addr;
    int     weight_bytes;
    int     bias_bytes;
} LayerQuant;

/* ── 网络状态 ────────────────────────────────────────────── */
typedef struct {
    uintptr_t ping;        // 当前输出 buffer
    uintptr_t pong;        // 下一层输出 buffer
    int       C, H, W;    // 当前 feature map 形状
} InferState;


/* ── T8 主推理接口 ───────────────────────────────────────── */
/* 模型: fastvit_t8_processed_128x128.onnx, 52 Conv 层 (0..51) */
int fastvit_t8_infer(
    const int8_t      *input,    /* [3 x 128 x 128] int8 */
    int8_t            *output,   /* [768 x 4 x 4]   int8 */
    const LayerWeight *lw,       /* 52 层权重地址 (0..51) */
    int8_t            *ping,     /* FV_FEAT_PING_BASE */
    int8_t            *pong);    /* FV_FEAT_PONG_BASE */

/* ── (旧版接口，保留兼容) ─────────────────────────────────── */
void fv_init(const char *weights_dir);
void fv_infer(const int8_t *input_img, int8_t *output_feat);

/* ── SE 模块软件实现 ──────────────────────────────────────
 * SE: ReduceMean → PWConv(squeeze) → ReLU → PWConv(excite) → Sigmoid → Mul
 */
void fv_se_block(
    int8_t *feat,         // [C, H, W] in-place 乘以 SE attention
    int C, int H, int W,
    const int8_t *w_sq,   // squeeze conv 权重 [C/4, C]
    const int32_t *b_sq,
    const int8_t *w_ex,   // excite  conv 权重 [C, C/4]
    const int32_t *b_ex,
    int shift_sq, int shift_ex
);

/* ── ReLU int8 (inline) ──────────────────────────────────── */
static inline void apply_relu(int8_t *data, int size)
{
    for (int i = 0; i < size; i++)
        if (data[i] < 0) data[i] = 0;
}

/* ── Channel-wise Mul (SE attention) ─────────────────────── */
static inline void apply_channel_scale(
    int8_t *feat, const int8_t *scale_vec,
    int C, int HW, int shift)
{
    for (int c = 0; c < C; c++) {
        int sc = scale_vec[c];
        for (int i = 0; i < HW; i++) {
            int32_t v = (int32_t)feat[c * HW + i] * sc;
            v = (v >> shift);
            feat[c * HW + i] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    }
}

#endif /* __FASTVIT_INFER_H__ */
