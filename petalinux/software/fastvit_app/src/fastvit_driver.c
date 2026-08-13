/*================================================================
 * fastvit_driver.c — Linux 版 (Petalinux)
 * AXI-Lite 寄存器通过 /dev/mem + mmap 访问
 * DMA 缓冲区位于物理保留内存段
 *
 * 合并后只有一个 IP (fastvit_ip)，只需一对 mmap 窗口，
 * 不再需要按 IP 区分的 if/else 轮询链。
 *================================================================*/

#include "fastvit_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

/* ── 映射的虚拟基地址 ─────────────────────────────────────── */
static int fd_mem = -1;
static volatile void *fv_ctrl  = NULL;  /* s_axi_control: 指针寄存器 */
static volatile void *fv_param = NULL;  /* s_axi_ctrl: 标量 + ap_ctrl */

/* ── 寄存器读写宏 (虚拟地址) ────────────────────────────── */
#define REG_WR(vbase, off, val) \
    (*(volatile uint32_t*)((char*)(vbase) + (off)) = (uint32_t)(val))
#define REG_RD(vbase, off) \
    (*(volatile uint32_t*)((char*)(vbase) + (off)))

static volatile void* do_mmap(uintptr_t phys_addr) {
    void *p = mmap(NULL, AXI_MAP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd_mem, phys_addr);
    if (p == MAP_FAILED) {
        perror("mmap AXI-Lite register");
        return NULL;
    }
    return p;
}

int fv_driver_init(void) {
    fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_mem < 0) {
        perror("open /dev/mem");
        return -1;
    }
    fv_ctrl  = do_mmap(FASTVIT_IP_CTRL_PHYS);
    fv_param = do_mmap(FASTVIT_IP_PARAM_PHYS);

    if (!fv_ctrl || !fv_param) {
        fprintf(stderr, "[fv_driver] mmap failed\n");
        return -1;
    }
    return 0;
}

void fv_driver_exit(void) {
    if (fv_ctrl)  munmap((void*)fv_ctrl,  AXI_MAP_SIZE);
    if (fv_param) munmap((void*)fv_param, AXI_MAP_SIZE);
    if (fd_mem >= 0) close(fd_mem);
}

/* ── 缓存同步 (Zynq HP 口非 cache-coherent) ─────────────── */
void fv_cache_flush(uintptr_t phys_addr, size_t size) {
    (void)phys_addr; (void)size;
    __builtin___clear_cache((char*)phys_addr, (char*)(phys_addr + size));
}

void fv_cache_invalidate(uintptr_t phys_addr, size_t size) {
    (void)phys_addr; (void)size;
    __builtin___clear_cache((char*)phys_addr, (char*)(phys_addr + size));
}

/* ── ap_done 轮询 ─────────────────────────────────────────── */
void fv_wait_done(void) {
    while (!(REG_RD(fv_param, AP_CTRL_OFFSET) & AP_DONE))
        ;
}

/* ── 64-bit 地址写入 (Zynq 32-bit AXI-Lite: 低/高各一个 32-bit 寄存器) */
static void w64(volatile void *base, uint32_t lo_off, uint32_t hi_off,
                uintptr_t phys_addr) {
    REG_WR(base, lo_off, (uint32_t)(phys_addr));
    REG_WR(base, hi_off, (uint32_t)((uint64_t)phys_addr >> 32));
}

/* ── conv_ip facade (OP_CONV) ─────────────────────────────── */
void fv_run_conv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win, int CHout,
    int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift)
{
    w64(fv_ctrl, FV_IN_A_LO, FV_IN_A_HI, feat_in);
    w64(fv_ctrl, FV_IN_B_LO, FV_IN_B_HI, weight);
    w64(fv_ctrl, FV_BIAS_LO, FV_BIAS_HI, bias);
    w64(fv_ctrl, FV_OUT_LO,  FV_OUT_HI,  feat_out);
    REG_WR(fv_param, FV_OP_CODE,   OP_CONV);
    REG_WR(fv_param, FV_CHIN,      CHin);
    REG_WR(fv_param, FV_HIN,       Hin);
    REG_WR(fv_param, FV_WIN,       Win);
    REG_WR(fv_param, FV_CHOUT,     CHout);
    REG_WR(fv_param, FV_ACT_MODE,  act_mode);
    REG_WR(fv_param, FV_OUT_SHIFT, out_shift);
    REG_WR(fv_param, FV_STRIDE_H,  stride_h);
    REG_WR(fv_param, FV_STRIDE_W,  stride_w);
    REG_WR(fv_param, FV_PAD_H,     pad_h);
    REG_WR(fv_param, FV_PAD_W,     pad_w);
    REG_WR(fv_param, AP_CTRL_OFFSET, AP_START);
    fv_wait_done();
}

/* ── dwconv_ip facade (OP_DWCONV) ─────────────────────────── */
void fv_run_dwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win,
    int Kh, int Kw, int stride_h, int stride_w, int pad_h, int pad_w,
    int fpg, int act_mode, int out_shift)
{
    w64(fv_ctrl, FV_IN_A_LO, FV_IN_A_HI, feat_in);
    w64(fv_ctrl, FV_IN_B_LO, FV_IN_B_HI, weight);
    w64(fv_ctrl, FV_BIAS_LO, FV_BIAS_HI, bias);
    w64(fv_ctrl, FV_OUT_LO,  FV_OUT_HI,  feat_out);
    REG_WR(fv_param, FV_OP_CODE,   OP_DWCONV);
    REG_WR(fv_param, FV_CHIN,      CHin);
    REG_WR(fv_param, FV_HIN,       Hin);
    REG_WR(fv_param, FV_WIN,       Win);
    REG_WR(fv_param, FV_KH,        Kh);
    REG_WR(fv_param, FV_KW,        Kw);
    REG_WR(fv_param, FV_STRIDE_H,  stride_h);
    REG_WR(fv_param, FV_STRIDE_W,  stride_w);
    REG_WR(fv_param, FV_PAD_H,     pad_h);
    REG_WR(fv_param, FV_PAD_W,     pad_w);
    REG_WR(fv_param, FV_FPG,       fpg);
    REG_WR(fv_param, FV_ACT_MODE,  act_mode);
    REG_WR(fv_param, FV_OUT_SHIFT, out_shift);
    REG_WR(fv_param, AP_CTRL_OFFSET, AP_START);
    fv_wait_done();
}

/* ── pwconv_ip facade (OP_PWCONV) ─────────────────────────── */
void fv_run_pwconv(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int H, int W, int CHout,
    int act_mode, int out_shift)
{
    w64(fv_ctrl, FV_IN_A_LO, FV_IN_A_HI, feat_in);
    w64(fv_ctrl, FV_IN_B_LO, FV_IN_B_HI, weight);
    w64(fv_ctrl, FV_BIAS_LO, FV_BIAS_HI, bias);
    w64(fv_ctrl, FV_OUT_LO,  FV_OUT_HI,  feat_out);
    REG_WR(fv_param, FV_OP_CODE,   OP_PWCONV);
    REG_WR(fv_param, FV_CHIN,      CHin);
    REG_WR(fv_param, FV_HIN,       H);
    REG_WR(fv_param, FV_WIN,       W);
    REG_WR(fv_param, FV_CHOUT,     CHout);
    REG_WR(fv_param, FV_ACT_MODE,  act_mode);
    REG_WR(fv_param, FV_OUT_SHIFT, out_shift);
    REG_WR(fv_param, AP_CTRL_OFFSET, AP_START);
    fv_wait_done();
}

/* ── add_ip facade (OP_ADD) ────────────────────────────────
 * in_b (second operand) shares the same pointer register as
 * weight for the other ops; bias is left untouched (unused by
 * add_worker in hardware). */
void fv_run_add(
    uintptr_t in_a, uintptr_t in_b, uintptr_t out,
    int CH, int H, int W)
{
    w64(fv_ctrl, FV_IN_A_LO, FV_IN_A_HI, in_a);
    w64(fv_ctrl, FV_IN_B_LO, FV_IN_B_HI, in_b);
    w64(fv_ctrl, FV_OUT_LO,  FV_OUT_HI,  out);
    REG_WR(fv_param, FV_OP_CODE, OP_ADD);
    REG_WR(fv_param, FV_CHIN,    CH);
    REG_WR(fv_param, FV_HIN,     H);
    REG_WR(fv_param, FV_WIN,     W);
    REG_WR(fv_param, AP_CTRL_OFFSET, AP_START);
    fv_wait_done();
}

/* ── gelu_worker facade (OP_GELU) ───────────────────────────
 * Elementwise LUT, single input/output -- in_b/bias untouched. */
void fv_run_gelu(
    uintptr_t in_a, uintptr_t out,
    int CH, int H, int W)
{
    w64(fv_ctrl, FV_IN_A_LO, FV_IN_A_HI, in_a);
    w64(fv_ctrl, FV_OUT_LO,  FV_OUT_HI,  out);
    REG_WR(fv_param, FV_OP_CODE, OP_GELU);
    REG_WR(fv_param, FV_CHIN,    CH);
    REG_WR(fv_param, FV_HIN,     H);
    REG_WR(fv_param, FV_WIN,     W);
    REG_WR(fv_param, AP_CTRL_OFFSET, AP_START);
    fv_wait_done();
}
