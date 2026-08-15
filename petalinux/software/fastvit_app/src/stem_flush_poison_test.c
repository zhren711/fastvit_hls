/*================================================================
 * stem_flush_poison_test.c — Phase 0.7 step 8.
 *
 * Step 7 (commit 2cd8374) pushed fv_cache_flush/invalidate into all
 * 52 fastvit_driver.c calls and end-to-end cosine got WORSE, not
 * better (0.4788 -> 0.0519). The commit message floated an unconfirmed
 * theory: fastvit_infer.c's very first op, `memcpy(cur, input,
 * 3*128*128)` at line 154 (writing the real test image into the Stem
 * input buffer), was never followed by a cache flush in ANY prior
 * version of the driver -- so Stem's fv_run_conv may have always read
 * stale/pre-existing DRAM content instead of the actual test image,
 * in every accuracy measurement ever taken on this project, including
 * the 0.4788 baseline.
 *
 * git show 2cd8374 confirms by inspection that fv_run_conv() (pre
 * step-7) never called fv_cache_flush(feat_in,...) internally, and no
 * call site in fastvit_infer.c added one either -- so the *code*
 * question is already answered. What code inspection can't answer is
 * the *hardware* question: did the 48KB dirty cache line range from
 * that memcpy happen to get evicted to DRAM anyway (e.g. from simple
 * capacity pressure, since a Cortex-A9 L1 D-cache is much smaller than
 * 48KB) before Stem's fv_run_conv triggered the IP? That can only be
 * answered by running it.
 *
 * This test isolates exactly that one variable, real hardware, single
 * measurement:
 *
 *   Test A ("historical"): poison the ping buffer (explicitly flushed
 *     first, so the poison is genuinely resident in DRAM, not just
 *     sitting in cache) -> memcpy the real test image over it, with NO
 *     flush call afterward, exactly matching every fastvit_infer.c
 *     that has ever run -> call a hand-reproduced pre-step-7 fv_run_conv
 *     (register pokes only, no internal fv_cache_flush(feat_in,...))
 *     -> invalidate + read the output.
 *   Test B ("explicit flush"): identical, except one line is added --
 *     fv_cache_flush() on the ping buffer right after the memcpy,
 *     before the same no-internal-flush conv call.
 *
 * Real physical addresses, real layer-0 (Stem conv) weights/bias/
 * out_shift, real test image, real IP -- everything is bit-identical
 * between A and B except that one flush call. If A and B produce
 * different Stem output stats, that confirms the IP was reading a
 * stale/poisoned ping buffer whenever the flush was skipped, i.e.
 * Stem never saw the real image in any prior accuracy measurement.
 * If A and B match, the theory is refuted for this specific case (the
 * dirty line range must be getting evicted/written back on its own).
 *================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "fastvit_driver.h"
#include "fastvit_infer.h"
#include "weights_layout.h"

#define FV_DDR_BASE       0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define FV_DMA_SIZE       0x06000000UL

static int fd_dma = -1;
static void *dma_base_virt = NULL;

static int dma_init(void) {
    fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem"); return -1; }
    dma_base_virt = mmap(NULL, FV_DMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_base_virt == MAP_FAILED) { perror("mmap DMA"); return -1; }
    return 0;
}

static void *phys_to_virt(uintptr_t phys) {
    return (char *)dma_base_virt + (phys - FV_DDR_BASE);
}

static int load_weights(const char *weights_dir) {
    uint8_t *ddr = (uint8_t *)phys_to_virt(FV_DDR_BASE);
    for (int i = 0; i < FV_NUM_LAYERS; i++) {
        if (FV_WEIGHT_SIZES[i] > 0 && FV_WEIGHT_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_WEIGHT_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            fread(ddr + FV_W_OFFSETS[i], 1, FV_WEIGHT_SIZES[i], f);
            fclose(f);
        }
        if (FV_BIAS_SIZES[i] > 0 && FV_BIAS_FILES[i][0] != '\0') {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s", weights_dir, FV_BIAS_FILES[i]);
            FILE *f = fopen(path, "rb");
            if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
            fread(ddr + FV_B_OFFSETS[i], 1, FV_BIAS_SIZES[i], f);
            fclose(f);
        }
    }
    msync(dma_base_virt, FV_WEIGHT_TOTAL_BYTES, MS_SYNC);
    return 0;
}

/* ── second, independent mmap of the fastvit_ip AXI-Lite windows ───
 * fastvit_driver.c's fv_ctrl/fv_param are file-static, so a
 * no-internal-flush conv call has to open its own mapping of the same
 * physical registers. /dev/mem MMIO mappings are uncached device
 * memory, so writes through this mapping are visible immediately to
 * fv_wait_done() polling through the driver's own mapping -- no
 * coherency concern, same as any two mmaps of the same peripheral. */
#define AP_CTRL_OFFSET  0x00
#define AP_START        (1u << 0)
#define FV_IN_A_LO 0x10
#define FV_IN_A_HI 0x14
#define FV_IN_B_LO 0x1C
#define FV_IN_B_HI 0x20
#define FV_BIAS_LO 0x28
#define FV_BIAS_HI 0x2C
#define FV_OUT_LO  0x34
#define FV_OUT_HI  0x38
#define FV_OP_CODE   0x10
#define FV_CHIN      0x18
#define FV_HIN       0x20
#define FV_WIN       0x28
#define FV_CHOUT     0x30
#define FV_ACT_MODE  0x38
#define FV_OUT_SHIFT 0x40
#define FV_STRIDE_H  0x48
#define FV_STRIDE_W  0x50
#define FV_PAD_H     0x58
#define FV_PAD_W     0x60
#define OP_CONV 0
#define ACT_NONE 0

#define REG_WR(vbase, off, val) \
    (*(volatile uint32_t*)((char*)(vbase) + (off)) = (uint32_t)(val))

static volatile void *my_ctrl = NULL, *my_param = NULL;

static int my_mmap_init(void) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem (regs)"); return -1; }
    my_ctrl  = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FASTVIT_IP_CTRL_PHYS);
    my_param = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FASTVIT_IP_PARAM_PHYS);
    close(fd);
    if (my_ctrl == MAP_FAILED || my_param == MAP_FAILED) { perror("mmap regs"); return -1; }
    return 0;
}

static void w64_local(volatile void *base, uint32_t lo, uint32_t hi, uintptr_t phys) {
    REG_WR(base, lo, (uint32_t)phys);
    REG_WR(base, hi, (uint32_t)((uint64_t)phys >> 32));
}

/* Exact reproduction of the pre-step-7 fv_run_conv() body (see
 * `git show 2cd8374 -- fastvit_driver.c`): no fv_cache_flush(feat_in,
 * weight, bias,...) calls at all. Uses the shared driver's
 * fv_wait_done() (declared in fastvit_driver.h) which polls through
 * the driver's own mapping -- safe, see comment above. */
static void fv_run_conv_noflush(
    uintptr_t feat_in, uintptr_t weight, uintptr_t bias, uintptr_t feat_out,
    int CHin, int Hin, int Win, int CHout,
    int stride_h, int stride_w, int pad_h, int pad_w,
    int act_mode, int out_shift)
{
    w64_local(my_ctrl, FV_IN_A_LO, FV_IN_A_HI, feat_in);
    w64_local(my_ctrl, FV_IN_B_LO, FV_IN_B_HI, weight);
    w64_local(my_ctrl, FV_BIAS_LO, FV_BIAS_HI, bias);
    w64_local(my_ctrl, FV_OUT_LO,  FV_OUT_HI,  feat_out);

    REG_WR(my_param, FV_OP_CODE,   OP_CONV);
    REG_WR(my_param, FV_CHIN,      CHin);
    REG_WR(my_param, FV_HIN,       Hin);
    REG_WR(my_param, FV_WIN,       Win);
    REG_WR(my_param, FV_CHOUT,     CHout);
    REG_WR(my_param, FV_ACT_MODE,  act_mode);
    REG_WR(my_param, FV_OUT_SHIFT, out_shift);
    REG_WR(my_param, FV_STRIDE_H,  stride_h);
    REG_WR(my_param, FV_STRIDE_W,  stride_w);
    REG_WR(my_param, FV_PAD_H,     pad_h);
    REG_WR(my_param, FV_PAD_W,     pad_w);
    REG_WR(my_param, AP_CTRL_OFFSET, AP_START);
    fv_wait_done();
}

/* ── stats ───────────────────────────────────────────────── */
static int8_t st_min(const int8_t *a, int n) { int8_t m=a[0]; for(int i=1;i<n;i++) if(a[i]<m) m=a[i]; return m; }
static int8_t st_max(const int8_t *a, int n) { int8_t m=a[0]; for(int i=1;i<n;i++) if(a[i]>m) m=a[i]; return m; }
static double st_mean(const int8_t *a, int n) { double s=0; for(int i=0;i<n;i++) s+=a[i]; return s/n; }
static int st_nonzero(const int8_t *a, int n) { int c=0; for(int i=0;i<n;i++) if(a[i]!=0) c++; return c; }

int main(int argc, char *argv[]) {
    const char *img_path = (argc > 1) ? argv[1] : "/home/root/accuracy_test_imgs/img_0000.bin";
    const char *weights_dir = (argc > 2) ? argv[2] : "/home/root/weights_t8";

    if (fv_driver_init() != 0) { fprintf(stderr, "fv_driver_init failed\n"); return 1; }
    if (dma_init() != 0) { return 1; }
    if (my_mmap_init() != 0) { return 1; }
    if (load_weights(weights_dir) != 0) { return 1; }

    static LayerWeight lw[FV_NUM_LAYERS];
    fv_build_layer_weights(lw, FV_DDR_BASE);

    int8_t *ping_virt = (int8_t *)phys_to_virt(FV_FEAT_PING_BASE);
    int8_t *pong_virt = (int8_t *)phys_to_virt(FV_FEAT_PONG_BASE);

    static int8_t real_image[3 * 128 * 128];
    static int8_t out_a[48 * 64 * 64];
    static int8_t out_b[48 * 64 * 64];

    FILE *f = fopen(img_path, "rb");
    if (!f) { perror("open image"); return 1; }
    size_t rd = fread(real_image, 1, sizeof(real_image), f);
    fclose(f);
    if (rd != sizeof(real_image)) {
        fprintf(stderr, "WARNING: read %zu bytes, expected %zu\n", rd, sizeof(real_image));
    }
    fprintf(stderr, "[image] %s: min=%d max=%d mean=%.3f\n",
            img_path, st_min(real_image, sizeof(real_image)), st_max(real_image, sizeof(real_image)),
            st_mean(real_image, sizeof(real_image)));

    const int CHin = 3, Hin = 128, Win = 128, CHout = 48;
    const int stride = 2, pad = 1, Hout = 64, Wout = 64;
    const size_t in_bytes = (size_t)CHin * Hin * Win;
    const size_t out_bytes = (size_t)CHout * Hout * Wout;

    /* ═══ Test A: historical -- poison, memcpy, NO flush, no-internal-flush conv ═══ */
    memset(ping_virt, 0x5A, in_bytes);
    fv_cache_flush((uintptr_t)FV_FEAT_PING_BASE, in_bytes);   /* guarantee poison is really in DRAM */
    memcpy(ping_virt, real_image, in_bytes);                   /* == fastvit_infer.c line 154, verbatim */
    /* NOTE: no flush here -- this is the historical code path */
    fv_run_conv_noflush((uintptr_t)FV_FEAT_PING_BASE, lw[0].w_addr, lw[0].b_addr,
                         (uintptr_t)FV_FEAT_PONG_BASE,
                         CHin, Hin, Win, CHout, stride, stride, pad, pad,
                         ACT_NONE, lw[0].out_shift);
    fv_cache_invalidate((uintptr_t)FV_FEAT_PONG_BASE, out_bytes);
    memcpy(out_a, pong_virt, out_bytes);
    fprintf(stderr, "[Test A: historical, no flush after memcpy]   min=%4d max=%4d mean=%8.3f nonzero=%d/%d\n",
            st_min(out_a, out_bytes), st_max(out_a, out_bytes), st_mean(out_a, out_bytes),
            st_nonzero(out_a, out_bytes), (int)out_bytes);

    /* ═══ Test B: same, but with an explicit flush after the memcpy ═══ */
    memset(ping_virt, 0x5A, in_bytes);
    fv_cache_flush((uintptr_t)FV_FEAT_PING_BASE, in_bytes);
    memcpy(ping_virt, real_image, in_bytes);
    fv_cache_flush((uintptr_t)FV_FEAT_PING_BASE, in_bytes);    /* <- the one variable that differs */
    fv_run_conv_noflush((uintptr_t)FV_FEAT_PING_BASE, lw[0].w_addr, lw[0].b_addr,
                         (uintptr_t)FV_FEAT_PONG_BASE,
                         CHin, Hin, Win, CHout, stride, stride, pad, pad,
                         ACT_NONE, lw[0].out_shift);
    fv_cache_invalidate((uintptr_t)FV_FEAT_PONG_BASE, out_bytes);
    memcpy(out_b, pong_virt, out_bytes);
    fprintf(stderr, "[Test B: explicit flush after memcpy]        min=%4d max=%4d mean=%8.3f nonzero=%d/%d\n",
            st_min(out_b, out_bytes), st_max(out_b, out_bytes), st_mean(out_b, out_bytes),
            st_nonzero(out_b, out_bytes), (int)out_bytes);

    int mismatches = 0;
    for (size_t i = 0; i < out_bytes; i++) if (out_a[i] != out_b[i]) mismatches++;
    fprintf(stderr, "[compare] A vs B mismatches: %d/%d (%.1f%%)\n",
            mismatches, (int)out_bytes, 100.0 * mismatches / out_bytes);

    FILE *fa = fopen("/home/root/stem_poison_out_a.bin", "wb");
    if (fa) { fwrite(out_a, 1, out_bytes, fa); fclose(fa); }
    FILE *fb = fopen("/home/root/stem_poison_out_b.bin", "wb");
    if (fb) { fwrite(out_b, 1, out_bytes, fb); fclose(fb); }

    fv_driver_exit();
    return 0;
}
