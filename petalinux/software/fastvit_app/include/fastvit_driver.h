/*================================================================
 * fastvit_driver.h  — Linux 版 (Petalinux)
 * AXI-Lite 寄存器通过 /dev/mem + mmap 访问
 *
 * 统一 fastvit_ip (合并 conv/dwconv/pwconv/add，op_code 分发)：
 *   fastvit_ip s_axi_control : 0x40000000  (指针寄存器)
 *   fastvit_ip s_axi_ctrl    : 0x40010000  (标量参数 + ap_ctrl)
 * (地址来自 run_impl_unified.tcl 的 assign_bd_address 输出，
 *  见 fastvit_bd.hwh 里 fastvit_ip_0 的 C_S_AXI_CONTROL/CTRL_BASEADDR)
 *
 * pool_ip 已彻底移除 (fv_run_pool 无调用方，SE块GlobalAvgPool 跑在ARM上)。
 *================================================================*/

#ifndef __FASTVIT_DRIVER_H__
#define __FASTVIT_DRIVER_H__

#include <stdint.h>
#include <stddef.h>

/* ── 物理基地址 (唯一一套，取代原来5个IP各2个窗口) ─────────── */
#define FASTVIT_IP_CTRL_PHYS   0x40000000UL  /* s_axi_control: 指针 */
#define FASTVIT_IP_PARAM_PHYS  0x40010000UL  /* s_axi_ctrl: 标量 + ap_ctrl */

/* 每个映射窗口大小 */
#define AXI_MAP_SIZE          0x10000UL

/* ── AP 控制寄存器偏移 ──────────────────────────────────────── */
#define AP_CTRL_OFFSET  0x00
#define AP_START  (1u << 0)
#define AP_DONE   (1u << 1)
#define AP_IDLE   (1u << 2)

/* ── fastvit_ip 寄存器偏移 (来自 Vitis HLS 导出的
 *   fastvit_ip_proj/solution1/impl/ip/drivers/fastvit_ip_v1_0/src/xfastvit_ip_hw.h) ── */

/* s_axi_control: 64-bit 指针 (低/高各一个 32-bit 寄存器) */
#define FV_IN_A_LO   0x10
#define FV_IN_A_HI   0x14
#define FV_IN_B_LO   0x1C
#define FV_IN_B_HI   0x20
#define FV_BIAS_LO   0x28
#define FV_BIAS_HI   0x2C
#define FV_OUT_LO    0x34
#define FV_OUT_HI    0x38

/* s_axi_ctrl: 标量参数 */
#define FV_OP_CODE    0x10
#define FV_CHIN       0x18
#define FV_HIN        0x20
#define FV_WIN        0x28
#define FV_CHOUT      0x30
#define FV_ACT_MODE   0x38
#define FV_OUT_SHIFT  0x40
#define FV_STRIDE_H   0x48
#define FV_STRIDE_W   0x50
#define FV_PAD_H      0x58
#define FV_PAD_W      0x60
#define FV_KH         0x68
#define FV_KW         0x70
#define FV_FPG        0x78

/* op_code (必须与 fastvit_ip/fastvit_ip.h 的枚举一致) */
#define OP_CONV    0
#define OP_DWCONV  1
#define OP_PWCONV  2
#define OP_ADD     3
#define OP_GELU    4

/* 激活函数 */
#define ACT_NONE  0
#define ACT_RELU  1

/* ── 驱动初始化/释放 ──────────────────────────────────────── */
int  fv_driver_init(void);   /* 映射 fastvit_ip 的两个寄存器窗口 */
void fv_driver_exit(void);   /* 释放映射 */

/* ── 缓存同步 (DMA 前后调用) ─────────────────────────────── */
void fv_cache_flush(uintptr_t phys_addr, size_t size);      /* CPU→FPGA */
void fv_cache_invalidate(uintptr_t phys_addr, size_t size); /* FPGA→CPU */

/* ── ap_done 轮询 ─────────────────────────────────────────── */
void fv_wait_done(void);

/* ── IP 调用接口 (物理地址) ──────────────────────────────────
 * 函数名/签名与合并前完全一致 (facade)，fastvit_infer.c 无需改动 —
 * 每个函数内部只是设置 op_code 并写入自己需要的那部分寄存器，
 * 再统一分发到底层唯一的 fastvit_ip。fv_run_pool 已删除 (无调用方)。 */
void fv_run_conv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win, int CHout,
    int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift
);

void fv_run_dwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win,
    int Kh, int Kw, int stride_h, int stride_w, int pad_h, int pad_w,
    int fpg, int act_mode, int out_shift
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

/* GELU on hardware (OP_GELU) -- in_a and out may be the same address
 * (in-place), matching how apply_gelu() was called on the ARM path. */
void fv_run_gelu(
    uintptr_t in_a, uintptr_t out,
    int CH, int H, int W
);

#endif /* __FASTVIT_DRIVER_H__ */
