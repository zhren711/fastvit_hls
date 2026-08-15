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
/* token_mixer(DW3) 输出保留区 (Phase 0.8 dataflow fix, 2026-08-15): DW3 的
 * 输出扇出给 DW7 和残差 Add 两个消费者，需要一块在 DW7->PW1->GELU->PW2 整个
 * 链路期间不被覆盖的独立缓冲区，ping/pong/temp 都会在此期间被覆写，不能复用。
 * 地址选在 ping 窗口内部 (0x12100000 + 1MB 偏移，ping/pong 相隔 2MB，全网
 * 最大单张特征图 196608 字节 = 0x30000，1MB 偏移留了 >3x 安全余量，不会和
 * ping 自己的内容重叠) 而不是在 temp 之后新开一个地址窗口——2026-08-15 用
 * 0x12700000 时 fv_run_add() 对这块地址静默不写入 (输出内容 = 调用前旧值)，
 * 疑似 fastvit_ip 里 add_worker 绑定的 m_axi master 的可寻址窗口没有覆盖到
 * 这个新地址 (5 个 worker 共享 4 个 m_axi master、op_code 分发，不同 worker
 * 绑不同 master，各自窗口独立声明)。落在 ping/pong/temp 已知可用的窗口内部
 * 规避这个问题，不需要新增地址窗口，也不需要额外的 cache 覆盖逻辑。 */
#define FV_FEAT_MIXED_BASE  0x12200000UL  /* inside the known-good ping window; address
                                            * theory tested and ruled out 2026-08-15, see
                                            * ZHR-8 -- kept here as the least-confounded
                                            * option pending further investigation */

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
