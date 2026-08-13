/*================================================================
 * dwconv_fpg_k3_isolate.c — Phase 0.6 isolated hardware test.
 *
 * Static code review of dwconv_worker.cpp's fpg loop + K=3/K=7
 * tiling didn't find an obvious bug, and weight/bias data + driver
 * register writes were already ruled out. This calls fv_run_dwconv()
 * DIRECTLY (bypassing the full 52-layer network) with tiny,
 * hand-computable inputs/weights, using the EXACT same shape
 * parameters as the real FinalDW call that produces all-zero output
 * (Kh=Kw=3, stride=2, pad=1, fpg=2), so real hardware behavior can be
 * compared against a hand-derived expected result byte-for-byte.
 *
 * Setup: CHin=2 (so fpg=2 -> CHout=4), Hin=Win=8 (same as FinalDW).
 * Input ch0[r,c] = r*8+c (0..63), ch1[r,c] = 100+r*8+c (100..163).
 * Per output channel, a single 1-tap weight (rest 0) picks out one
 * specific input pixel per output position, so the expected output is
 * just a sampled/shifted copy of the input -- easy to hand-verify:
 *
 *   co=0 (ch=0,f=0): tap (kh=1,kw=1) center -> out[oh,ow] = in_ch0[2*oh,   2*ow]     (no padding hit)
 *   co=1 (ch=0,f=1): tap (kh=0,kw=0) top-left -> out[oh,ow] = in_ch0[2*oh-1, 2*ow-1]   (hits zero-pad when oh=0 or ow=0)
 *   co=2 (ch=1,f=0): same as co=0 but on ch1
 *   co=3 (ch=1,f=1): same as co=1 but on ch1
 *
 * bias=0, out_shift=0 (values stay well inside int8 range).
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
#define DMA_SIZE     0x00100000UL  /* 1MB is plenty for this tiny test */

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

    /* Physical layout, all tiny, all within the 1MB window */
    uintptr_t IN_PHYS   = FV_DDR_BASE + 0x0000;   /* 2 * 8*8 = 128 bytes */
    uintptr_t WT_PHYS    = FV_DDR_BASE + 0x1000;   /* 4 * 9 = 36 bytes */
    uintptr_t BIAS_PHYS  = FV_DDR_BASE + 0x2000;   /* 4 * 4 = 16 bytes */
    uintptr_t OUT_PHYS   = FV_DDR_BASE + 0x3000;   /* 4 * 4*4 = 64 bytes */

    int8_t  *in_v   = (int8_t *)phys_to_virt(IN_PHYS);
    int8_t  *wt_v   = (int8_t *)phys_to_virt(WT_PHYS);
    int32_t *bias_v = (int32_t *)phys_to_virt(BIAS_PHYS);
    int8_t  *out_v  = (int8_t *)phys_to_virt(OUT_PHYS);

    const int CHin = 2, Hin = 8, Win = 8, Kh = 3, Kw = 3, fpg = 2;
    const int stride = 2, pad = 1;
    const int Hout = 4, Wout = 4;
    const int CHout = CHin * fpg;

    /* input: ch*64 + r*8 + c */
    for (int ch = 0; ch < CHin; ch++)
        for (int r = 0; r < Hin; r++)
            for (int c = 0; c < Win; c++)
                in_v[ch * Hin * Win + r * Win + c] = (int8_t)(ch * 100 + r * Win + c);

    /* weight: [CHout][Kh][Kw], f=0 -> center tap(1,1)=1, f=1 -> top-left tap(0,0)=1 */
    memset(wt_v, 0, CHout * Kh * Kw);
    for (int ch = 0; ch < CHin; ch++) {
        int co0 = ch * fpg + 0;
        int co1 = ch * fpg + 1;
        wt_v[co0 * Kh * Kw + 1 * Kw + 1] = 1;  /* center */
        wt_v[co1 * Kh * Kw + 0 * Kw + 0] = 1;  /* top-left */
    }
    for (int co = 0; co < CHout; co++) bias_v[co] = 0;

    memset(out_v, 0xAA, CHout * Hout * Wout);  /* poison pattern, so leftover 0xAA proves "never written" vs a real computed 0 */

    fv_cache_flush(IN_PHYS, CHin * Hin * Win);
    fv_cache_flush(WT_PHYS, CHout * Kh * Kw);
    fv_cache_flush(BIAS_PHYS, CHout * 4);

    fv_run_dwconv(IN_PHYS, WT_PHYS, BIAS_PHYS, OUT_PHYS,
                  CHin, Hin, Win, Kh, Kw, stride, stride, pad, pad,
                  fpg, ACT_NONE, /*out_shift=*/0);

    fv_cache_invalidate(OUT_PHYS, CHout * Hout * Wout);

    /* Hand-computed expected output */
    int8_t expect[4 * 4 * 4];
    for (int ch = 0; ch < CHin; ch++) {
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;
            for (int oh = 0; oh < Hout; oh++) {
                for (int ow = 0; ow < Wout; ow++) {
                    int ih, iw;
                    if (f == 0) { ih = oh * stride - pad + 1; iw = ow * stride - pad + 1; }  /* center tap */
                    else        { ih = oh * stride - pad + 0; iw = ow * stride - pad + 0; }  /* top-left tap */
                    int8_t v = (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                               ? (int8_t)(ch * 100 + ih * Win + iw) : 0;
                    expect[co * Hout * Wout + oh * Wout + ow] = v;
                }
            }
        }
    }

    printf("=== dwconv fpg=2 K=3 isolated test (Hin=Win=8, stride=2, pad=1, matching real FinalDW shape) ===\n");
    int mismatches = 0;
    for (int co = 0; co < CHout; co++) {
        printf("co=%d (ch=%d,f=%d):\n", co, co / fpg, co % fpg);
        for (int oh = 0; oh < Hout; oh++) {
            printf("  row%d  got:", oh);
            for (int ow = 0; ow < Wout; ow++)
                printf(" %4d", (int)out_v[co * Hout * Wout + oh * Wout + ow]);
            printf("   expect:");
            for (int ow = 0; ow < Wout; ow++)
                printf(" %4d", (int)expect[co * Hout * Wout + oh * Wout + ow]);
            int row_mismatch = 0;
            for (int ow = 0; ow < Wout; ow++)
                if (out_v[co * Hout * Wout + oh * Wout + ow] != expect[co * Hout * Wout + oh * Wout + ow])
                    row_mismatch = 1;
            printf(row_mismatch ? "   <-- MISMATCH\n" : "\n");
            if (row_mismatch) mismatches++;
        }
    }
    printf("=== %s (%d mismatching rows / %d total) ===\n",
           mismatches == 0 ? "ALL MATCH" : "MISMATCHES FOUND", mismatches, CHout * Hout);

    munmap(dma_virt, DMA_SIZE);
    close(fd_dma);
    fv_driver_exit();
    return mismatches != 0;
}
