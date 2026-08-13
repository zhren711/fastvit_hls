/**
 * fastvit_driver.h
 * FastVIT FPGA 加速器 ARM 驱动
 * 寄存器地址直接来自 HLS IP 生成的 xparameters.h
 *
 * AXI-Lite 基地址 (由 Vivado assign_bd_address 分配):
 *   add_ip   s_axi_control : 0x40000000
 *   add_ip   s_axi_ctrl    : 0x40010000
 *   conv_ip  s_axi_control : 0x40020000
 *   conv_ip  s_axi_ctrl    : 0x40030000
 *   dwconv_ip s_axi_control: 0x40040000
 *   dwconv_ip s_axi_ctrl   : 0x40050000
 *   pool_ip  s_axi_control : 0x40060000
 *   pool_ip  s_axi_ctrl    : 0x40070000
 *   pwconv_ip s_axi_control: 0x40080000
 *   pwconv_ip s_axi_ctrl   : 0x40090000
 *
 * 规则:
 *   s_axi_control: 存 DMA 指针（feat_in/weight/bias/feat_out 地址）
 *   s_axi_ctrl:    存标量参数（CHin/H/W 等）+ ap_ctrl
 */

#ifndef __FASTVIT_DRIVER_H__
#define __FASTVIT_DRIVER_H__

#include <stdint.h>

/* ── AXI-Lite 基地址 ─────────────────────────────────────── */
#define ADD_IP_CTRL_BASE      0x40000000UL
#define ADD_IP_PARAM_BASE     0x40010000UL
#define CONV_IP_CTRL_BASE     0x40020000UL
#define CONV_IP_PARAM_BASE    0x40030000UL
#define DWCONV_IP_CTRL_BASE   0x40040000UL
#define DWCONV_IP_PARAM_BASE  0x40050000UL
#define POOL_IP_CTRL_BASE     0x40060000UL
#define POOL_IP_PARAM_BASE    0x40070000UL
#define PWCONV_IP_CTRL_BASE   0x40080000UL
#define PWCONV_IP_PARAM_BASE  0x40090000UL

/* ── AP 控制寄存器 (s_axi_ctrl offset) ─────────────────────
 * 注意: AP_CTRL 在 s_axi_ctrl（标量口），不在 s_axi_control
 */
#define AP_CTRL_OFFSET  0x00
#define AP_GIE_OFFSET   0x04
#define AP_IER_OFFSET   0x08
#define AP_ISR_OFFSET   0x0C

#define AP_START  (1u << 0)
#define AP_DONE   (1u << 1)
#define AP_IDLE   (1u << 2)
#define AP_READY  (1u << 3)

/* ── add_ip 寄存器 ───────────────────────────────────────────
 * s_axi_control (ADD_IP_CTRL_BASE):
 *   feat_in1: 0x10/0x14 (低/高32位)
 *   feat_in2: 0x1c/0x20
 *   feat_out: 0x28/0x2c
 * s_axi_ctrl (ADD_IP_PARAM_BASE):
 *   ap_ctrl: 0x00
 *   CH:      0x10
 *   H:       0x18
 *   W:       0x20
 */
#define ADD_FEAT_IN1_LO   0x10
#define ADD_FEAT_IN1_HI   0x14
#define ADD_FEAT_IN2_LO   0x1C
#define ADD_FEAT_IN2_HI   0x20
#define ADD_FEAT_OUT_LO   0x28
#define ADD_FEAT_OUT_HI   0x2C
#define ADD_CH            0x10   /* s_axi_ctrl 偏移 */
#define ADD_H             0x18
#define ADD_W             0x20

/* ── conv_ip 寄存器 ──────────────────────────────────────────
 * s_axi_control (CONV_IP_CTRL_BASE):
 *   feat_in:  0x10/0x14
 *   weight:   0x1c/0x20
 *   bias:     0x28/0x2c
 *   feat_out: 0x34/0x38
 * s_axi_ctrl (CONV_IP_PARAM_BASE):
 *   CHin:0x10 Hin:0x18 Win:0x20 CHout:0x28
 *   stride_h:0x30 stride_w:0x38 pad_h:0x40 pad_w:0x48
 *   act_mode:0x50 out_shift:0x58
 * 注意: conv_ip 固定 K=3, 无 Kh/Kw/mode 参数
 */
#define CONV_FEAT_IN_LO   0x10
#define CONV_FEAT_IN_HI   0x14
#define CONV_WEIGHT_LO    0x1C
#define CONV_WEIGHT_HI    0x20
#define CONV_BIAS_LO      0x28
#define CONV_BIAS_HI      0x2C
#define CONV_FEAT_OUT_LO  0x34
#define CONV_FEAT_OUT_HI  0x38
#define CONV_CHIN         0x10
#define CONV_HIN          0x18
#define CONV_WIN          0x20
#define CONV_CHOUT        0x28
#define CONV_STRIDE_H     0x30
#define CONV_STRIDE_W     0x38
#define CONV_PAD_H        0x40
#define CONV_PAD_W        0x48
#define CONV_ACT_MODE     0x50
#define CONV_OUT_SHIFT    0x58

/* ── dwconv_ip v2 寄存器 ─────────────────────────────────────
 * 接口与 v1 兼容 (stride/pad_h/pad_w 保留), 内部 stride=1
 * s_axi_control: feat_in:0x10 weight:0x1c bias:0x28 feat_out:0x34
 * s_axi_ctrl: CHin:0x10 Hin:0x18 Win:0x20 Kh:0x28 Kw:0x30
 *             stride_h:0x38 stride_w:0x40 pad_h:0x48 pad_w:0x50
 *             act_mode:0x58 out_shift:0x60
 */
#define DW_FEAT_IN_LO     0x10
#define DW_FEAT_IN_HI     0x14
#define DW_WEIGHT_LO      0x1C
#define DW_WEIGHT_HI      0x20
#define DW_BIAS_LO        0x28
#define DW_BIAS_HI        0x2C
#define DW_FEAT_OUT_LO    0x34
#define DW_FEAT_OUT_HI    0x38
/* 标量参数 (s_axi_ctrl 偏移) */
#define DW_CHIN           0x10
#define DW_HIN            0x18
#define DW_WIN            0x20
#define DW_KH             0x28
#define DW_KW             0x30
#define DW_STRIDE_H       0x38
#define DW_STRIDE_W       0x40
#define DW_PAD_H          0x48
#define DW_PAD_W          0x50
#define DW_ACT_MODE       0x58
#define DW_OUT_SHIFT      0x60

/* ── pwconv_ip 寄存器 ────────────────────────────────────────
 * s_axi_control: feat_in:0x10 weight:0x1c bias:0x28 feat_out:0x34
 * s_axi_ctrl: CHin:0x10 H:0x18 W:0x20 CHout:0x28
 *             act_mode:0x30 out_shift:0x38
 */
#define PW_FEAT_IN_LO     0x10
#define PW_FEAT_IN_HI     0x14
#define PW_WEIGHT_LO      0x1C
#define PW_WEIGHT_HI      0x20
#define PW_BIAS_LO        0x28
#define PW_BIAS_HI        0x2C
#define PW_FEAT_OUT_LO    0x34
#define PW_FEAT_OUT_HI    0x38
#define PW_CHIN           0x10
#define PW_H              0x18
#define PW_W              0x20
#define PW_CHOUT          0x28
#define PW_ACT_MODE       0x30
#define PW_OUT_SHIFT      0x38

/* ── pool_ip 寄存器 ──────────────────────────────────────────
 * s_axi_control: feat_in:0x10 feat_out:0x1c  (无 bias)
 * s_axi_ctrl: CH:0x10 Hin:0x18 Win:0x20 Kh:0x28 Kw:0x30
 *             stride_h:0x38 stride_w:0x40 pad_h:0x48 pad_w:0x50
 *             mode:0x58 out_shift:0x60
 */
#define POOL_FEAT_IN_LO   0x10
#define POOL_FEAT_IN_HI   0x14
#define POOL_FEAT_OUT_LO  0x1C
#define POOL_FEAT_OUT_HI  0x20
#define POOL_CH           0x10
#define POOL_HIN          0x18
#define POOL_WIN          0x20
#define POOL_KH           0x28
#define POOL_KW           0x30
#define POOL_STRIDE_H     0x38
#define POOL_STRIDE_W     0x40
#define POOL_PAD_H        0x48
#define POOL_PAD_W        0x50
#define POOL_MODE         0x58
#define POOL_OUT_SHIFT    0x60

#define POOL_MODE_MAX     0
#define POOL_MODE_AVG     1
#define POOL_MODE_GLOBAL  2

/* ── 激活函数 ────────────────────────────────────────────── */
#define ACT_NONE  0
#define ACT_RELU  1

/* ── 寄存器读写宏 ────────────────────────────────────────── */
#define REG_WR(base, off, val) \
    (*(volatile uint32_t *)((uintptr_t)(base) + (off)) = (uint32_t)(val))
#define REG_RD(base, off) \
    (*(volatile uint32_t *)((uintptr_t)(base) + (off)))

/* ── 函数声明 ────────────────────────────────────────────── */
void fv_wait_done(uintptr_t param_base);

void fv_run_conv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win, int CHout,
    int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift
);

/* v2: stride_h/stride_w 接口兼容保留, 内部固定 stride=1 */
void fv_run_dwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win,
    int Kh, int Kw, int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift
);

void fv_run_pwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int H, int W, int CHout,
    int act_mode, int out_shift
);

void fv_run_add(
    uintptr_t in_a, uintptr_t in_b, uintptr_t out,
    int CH, int H, int W
);

void fv_run_pool(
    uintptr_t feat_in, uintptr_t feat_out,
    int CH, int Hin, int Win,
    int mode, int Kh, int Kw,
    int stride_h, int stride_w
);

#endif /* __FASTVIT_DRIVER_H__ */
