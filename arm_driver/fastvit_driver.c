/**
 * fastvit_driver.c - FastVIT IP 驱动实现
 * 注意: s_axi_control 写 DMA 指针地址
 *       s_axi_ctrl    写标量参数 + ap_start/done
 */
#include "fastvit_driver.h"

static inline void w64(uintptr_t base, uint32_t lo_off, uint32_t hi_off, uintptr_t addr)
{
    REG_WR(base, lo_off, (uint32_t)(addr));
    REG_WR(base, hi_off, (uint32_t)((uint64_t)addr >> 32));
}

/* ap_ctrl 在 s_axi_ctrl（参数口），offset 0x00 */
void fv_wait_done(uintptr_t param_base)
{
    while (!(REG_RD(param_base, AP_CTRL_OFFSET) & AP_DONE))
        ;
}

/* ── conv_ip ──────────────────────────────────────────────── */
void fv_run_conv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win, int CHout,
    int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift)
{
    /* 指针 → s_axi_control */
    w64(CONV_IP_CTRL_BASE,  CONV_FEAT_IN_LO,  CONV_FEAT_IN_HI,  feat_in);
    w64(CONV_IP_CTRL_BASE,  CONV_WEIGHT_LO,   CONV_WEIGHT_HI,   weight);
    w64(CONV_IP_CTRL_BASE,  CONV_BIAS_LO,     CONV_BIAS_HI,     bias);
    w64(CONV_IP_CTRL_BASE,  CONV_FEAT_OUT_LO, CONV_FEAT_OUT_HI, feat_out);
    /* 标量 → s_axi_ctrl (conv_ip 固定 K=3, 无 Kh/Kw/mode 寄存器) */
    REG_WR(CONV_IP_PARAM_BASE, CONV_CHIN,      CHin);
    REG_WR(CONV_IP_PARAM_BASE, CONV_HIN,       Hin);
    REG_WR(CONV_IP_PARAM_BASE, CONV_WIN,       Win);
    REG_WR(CONV_IP_PARAM_BASE, CONV_CHOUT,     CHout);
    REG_WR(CONV_IP_PARAM_BASE, CONV_STRIDE_H,  stride_h);
    REG_WR(CONV_IP_PARAM_BASE, CONV_STRIDE_W,  stride_w);
    REG_WR(CONV_IP_PARAM_BASE, CONV_PAD_H,     pad_h);
    REG_WR(CONV_IP_PARAM_BASE, CONV_PAD_W,     pad_w);
    REG_WR(CONV_IP_PARAM_BASE, CONV_ACT_MODE,  act_mode);
    REG_WR(CONV_IP_PARAM_BASE, CONV_OUT_SHIFT, out_shift);
    /* 启动 */
    REG_WR(CONV_IP_PARAM_BASE, AP_CTRL_OFFSET, AP_START);
    fv_wait_done(CONV_IP_PARAM_BASE);
}

/* ── dwconv_ip v2 (接口兼容 v1) ─────────────────────────── */
void fv_run_dwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win,
    int Kh, int Kw, int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift)
{
    w64(DWCONV_IP_CTRL_BASE,  DW_FEAT_IN_LO,  DW_FEAT_IN_HI,  feat_in);
    w64(DWCONV_IP_CTRL_BASE,  DW_WEIGHT_LO,   DW_WEIGHT_HI,   weight);
    w64(DWCONV_IP_CTRL_BASE,  DW_BIAS_LO,     DW_BIAS_HI,     bias);
    w64(DWCONV_IP_CTRL_BASE,  DW_FEAT_OUT_LO, DW_FEAT_OUT_HI, feat_out);

    REG_WR(DWCONV_IP_PARAM_BASE, DW_CHIN,      CHin);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_HIN,       Hin);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_WIN,       Win);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_KH,        Kh);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_KW,        Kw);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_STRIDE_H,  stride_h);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_STRIDE_W,  stride_w);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_PAD_H,     pad_h);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_PAD_W,     pad_w);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_ACT_MODE,  act_mode);
    REG_WR(DWCONV_IP_PARAM_BASE, DW_OUT_SHIFT, out_shift);

    REG_WR(DWCONV_IP_PARAM_BASE, AP_CTRL_OFFSET, AP_START);
    fv_wait_done(DWCONV_IP_PARAM_BASE);
}

/* ── pwconv_ip ────────────────────────────────────────────── */
void fv_run_pwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int H, int W, int CHout,
    int act_mode, int out_shift)
{
    w64(PWCONV_IP_CTRL_BASE,  PW_FEAT_IN_LO,  PW_FEAT_IN_HI,  feat_in);
    w64(PWCONV_IP_CTRL_BASE,  PW_WEIGHT_LO,   PW_WEIGHT_HI,   weight);
    w64(PWCONV_IP_CTRL_BASE,  PW_BIAS_LO,     PW_BIAS_HI,     bias);
    w64(PWCONV_IP_CTRL_BASE,  PW_FEAT_OUT_LO, PW_FEAT_OUT_HI, feat_out);

    REG_WR(PWCONV_IP_PARAM_BASE, PW_CHIN,      CHin);
    REG_WR(PWCONV_IP_PARAM_BASE, PW_H,         H);
    REG_WR(PWCONV_IP_PARAM_BASE, PW_W,         W);
    REG_WR(PWCONV_IP_PARAM_BASE, PW_CHOUT,     CHout);
    REG_WR(PWCONV_IP_PARAM_BASE, PW_ACT_MODE,  act_mode);
    REG_WR(PWCONV_IP_PARAM_BASE, PW_OUT_SHIFT, out_shift);

    REG_WR(PWCONV_IP_PARAM_BASE, AP_CTRL_OFFSET, AP_START);
    fv_wait_done(PWCONV_IP_PARAM_BASE);
}

/* ── add_ip ───────────────────────────────────────────────── */
void fv_run_add(
    uintptr_t in_a, uintptr_t in_b, uintptr_t out,
    int CH, int H, int W)
{
    w64(ADD_IP_CTRL_BASE, ADD_FEAT_IN1_LO, ADD_FEAT_IN1_HI, in_a);
    w64(ADD_IP_CTRL_BASE, ADD_FEAT_IN2_LO, ADD_FEAT_IN2_HI, in_b);
    w64(ADD_IP_CTRL_BASE, ADD_FEAT_OUT_LO, ADD_FEAT_OUT_HI, out);

    REG_WR(ADD_IP_PARAM_BASE, ADD_CH, CH);
    REG_WR(ADD_IP_PARAM_BASE, ADD_H,  H);
    REG_WR(ADD_IP_PARAM_BASE, ADD_W,  W);

    REG_WR(ADD_IP_PARAM_BASE, AP_CTRL_OFFSET, AP_START);
    fv_wait_done(ADD_IP_PARAM_BASE);
}

/* ── pool_ip ──────────────────────────────────────────────── */
void fv_run_pool(
    uintptr_t feat_in, uintptr_t feat_out,
    int CH, int Hin, int Win,
    int mode, int Kh, int Kw,
    int stride_h, int stride_w)
{
    w64(POOL_IP_CTRL_BASE, POOL_FEAT_IN_LO,  POOL_FEAT_IN_HI,  feat_in);
    w64(POOL_IP_CTRL_BASE, POOL_FEAT_OUT_LO, POOL_FEAT_OUT_HI, feat_out);

    REG_WR(POOL_IP_PARAM_BASE, POOL_CH,       CH);
    REG_WR(POOL_IP_PARAM_BASE, POOL_HIN,      Hin);
    REG_WR(POOL_IP_PARAM_BASE, POOL_WIN,      Win);
    REG_WR(POOL_IP_PARAM_BASE, POOL_KH,       Kh);
    REG_WR(POOL_IP_PARAM_BASE, POOL_KW,       Kw);
    REG_WR(POOL_IP_PARAM_BASE, POOL_STRIDE_H, stride_h);
    REG_WR(POOL_IP_PARAM_BASE, POOL_STRIDE_W, stride_w);
    REG_WR(POOL_IP_PARAM_BASE, POOL_MODE,     mode);

    REG_WR(POOL_IP_PARAM_BASE, AP_CTRL_OFFSET, AP_START);
    fv_wait_done(POOL_IP_PARAM_BASE);
}
