/*================================================================
 * dwconv_fpg_k3_realaddr.c — Phase 0.6 fourth follow-up.
 *
 * Ruled out so far: fpg=2+K=3 interaction (tiny + full scale), op_code
 * warm-up/switching, dense 9-tap accumulation -- ALL passed cleanly at
 * addresses close to FV_DDR_BASE (low offsets, 0x10000-0x84000ish).
 * The one thing never controlled for: the REAL FinalDW call uses
 * addresses far from FV_DDR_BASE -- ping/pong feature buffers at
 * FV_DDR_BASE+0x02100000/0x02300000 (~33/35MB in), and its own real
 * weight/bias offsets ~2.6MB/3.0MB in (FV_W_OFFSETS[49]/FV_B_OFFSETS[49]
 * from weights_layout.h). This test uses those EXACT real physical
 * addresses and the REAL weight+bias bytes for layer 49 (copied from
 * the actual weights_t8/layer_0049_dwconv_{weight,bias}.bin files,
 * already downloaded and confirmed healthy in an earlier round), with
 * an all-ZERO input -- so only bias survives the conv (each output
 * channel's expected value collapses to clamp(bias[co] >> out_shift),
 * independent of the weight values entirely), which is trivial to
 * hand-verify while still using the exact real memory addresses.
 *================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "fastvit_driver.h"

#define FV_DDR_BASE  0x10000000UL
#define FV_FEAT_PING_BASE 0x12100000UL
#define FV_FEAT_PONG_BASE 0x12300000UL
#define DMA_SIZE     0x06000000UL  /* same 96MB window main.c uses */

/* Real values for layer 49 (FinalDW), from weights_layout.h */
#define L49_W_OFFSET 2613408U
#define L49_B_OFFSET 3067488U
#define L49_OUT_SHIFT 8

static int fd_dma = -1;
static void *dma_virt = NULL;

static void *phys_to_virt(uintptr_t phys) {
    return (char *)dma_virt + (phys - FV_DDR_BASE);
}

int main(void) {
    if (fv_driver_init() != 0) { fprintf(stderr, "driver init failed\n"); return 1; }

    fd_dma = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd_dma < 0) { perror("open /dev/mem"); return 1; }
    dma_virt = mmap(NULL, DMA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd_dma, FV_DDR_BASE);
    if (dma_virt == MAP_FAILED) { perror("mmap"); return 1; }

    const int CHin = 384, Hin = 8, Win = 8, Kh = 3, Kw = 3, fpg = 2;
    const int stride = 2, pad = 1;
    const int Hout = 4, Wout = 4;
    const int CHout = CHin * fpg;

    uintptr_t IN_PHYS   = FV_FEAT_PING_BASE;              /* REAL ping address */
    uintptr_t OUT_PHYS  = FV_FEAT_PONG_BASE;              /* REAL pong address */
    uintptr_t WT_PHYS   = FV_DDR_BASE + L49_W_OFFSET;     /* REAL weight offset */
    uintptr_t BIAS_PHYS = FV_DDR_BASE + L49_B_OFFSET;     /* REAL bias offset */

    int8_t  *in_v   = (int8_t *)phys_to_virt(IN_PHYS);
    int8_t  *wt_v   = (int8_t *)phys_to_virt(WT_PHYS);
    int32_t *bias_v = (int32_t *)phys_to_virt(BIAS_PHYS);
    int8_t  *out_v  = (int8_t *)phys_to_virt(OUT_PHYS);

    /* Load the REAL weight/bias bytes for layer 49 from the actual
     * weights_t8 dir on the board (same files the production binary
     * uses), not synthetic patterns. */
    FILE *fw = fopen("/home/root/weights_t8/layer_0049_dwconv_weight.bin", "rb");
    if (!fw) { perror("open weight file"); return 1; }
    size_t wr = fread(wt_v, 1, CHout * Kh * Kw, fw);
    fclose(fw);
    FILE *fb = fopen("/home/root/weights_t8/layer_0049_dwconv_bias.bin", "rb");
    if (!fb) { perror("open bias file"); return 1; }
    size_t br = fread(bias_v, 1, CHout * 4, fb);
    fclose(fb);
    printf("loaded real weight (%zu B) + bias (%zu B) for layer 49\n", wr, br);

    memset(in_v, 0, (size_t)CHin * Hin * Win);   /* all-zero input -> only bias survives */
    memset(out_v, 0xAA, (size_t)CHout * Hout * Wout);

    fv_cache_flush(IN_PHYS, (size_t)CHin * Hin * Win);
    fv_cache_flush(WT_PHYS, (size_t)CHout * Kh * Kw);
    fv_cache_flush(BIAS_PHYS, (size_t)CHout * 4);

    printf("calling fv_run_dwconv at REAL addresses: in=0x%lx wt=0x%lx bias=0x%lx out=0x%lx\n",
           (unsigned long)IN_PHYS, (unsigned long)WT_PHYS, (unsigned long)BIAS_PHYS, (unsigned long)OUT_PHYS);
    fv_run_dwconv(IN_PHYS, WT_PHYS, BIAS_PHYS, OUT_PHYS,
                  CHin, Hin, Win, Kh, Kw, stride, stride, pad, pad,
                  fpg, ACT_NONE, L49_OUT_SHIFT);

    fv_cache_invalidate(OUT_PHYS, (size_t)CHout * Hout * Wout);

    /* Expected: input=0 everywhere, so conv sum=0 regardless of
     * weight -> output[co] = clamp(bias[co] >> 8) for all 16 positions. */
    int mismatching_channels = 0, all_zero_channels = 0, nonzero_expect_but_zero_got = 0;
    for (int co = 0; co < CHout; co++) {
        int32_t acc = bias_v[co];
        int32_t s = acc >> L49_OUT_SHIFT;
        int8_t expect = (int8_t)(s > 127 ? 127 : s < -128 ? -128 : s);
        int bad = 0, all_zero = 1;
        for (int i = 0; i < Hout * Wout; i++) {
            int8_t got = out_v[co * Hout * Wout + i];
            if (got != expect) bad = 1;
            if (got != 0) all_zero = 0;
        }
        if (bad) mismatching_channels++;
        if (all_zero) all_zero_channels++;
        if (expect != 0 && all_zero) nonzero_expect_but_zero_got++;
        if (co < 5 || bad) {
            printf("co=%3d bias=%8d expect=%4d got[0..3]=%4d %4d %4d %4d %s\n",
                   co, bias_v[co], expect,
                   (int)out_v[co*16+0], (int)out_v[co*16+1], (int)out_v[co*16+2], (int)out_v[co*16+3],
                   bad ? "<-- MISMATCH" : "");
        }
    }
    printf("mismatching channels: %d / %d\n", mismatching_channels, CHout);
    printf("all-zero-output channels: %d / %d\n", all_zero_channels, CHout);
    printf("channels where bias implies nonzero but hw gave all-zero: %d / %d\n",
           nonzero_expect_but_zero_got, CHout);
    printf("=== %s ===\n", mismatching_channels == 0 ? "ALL MATCH (bug NOT reproduced)" : "MISMATCHES FOUND (bug reproduced!)");

    munmap(dma_virt, DMA_SIZE);
    close(fd_dma);
    fv_driver_exit();
    return mismatching_channels != 0;
}
