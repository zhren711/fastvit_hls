/*================================================================
 * dwconv_fpg_k3_scale384.c — Phase 0.6 follow-up isolated test.
 *
 * dwconv_fpg_k3_isolate.c (CHin=2, fpg=2, K=3, same Hin/Win/stride/pad
 * as the real broken FinalDW call) passed PERFECTLY -- 16/16 rows
 * matched hand-computed expected values. That rules out "fpg=2+K=3"
 * as a general interaction bug. The one thing that test did NOT
 * reproduce is scale: real FinalDW has CHin=384 (CHout=768), the
 * largest CHout of any dwconv call in the whole network (Trans1/2/3
 * are fpg=2 but CHin<=192; Stage4's CHin=384 calls are fpg=1). This
 * rerun keeps the exact same hand-verifiable 1-tap weight trick but
 * scales CHin up to 384 to test whether scale, not fpg+K3, is what
 * actually triggers the bug.
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
#define DMA_SIZE     0x00200000UL  /* 2MB -- input alone is 384*64=24576B, plenty of margin */

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
    const int CHout = CHin * fpg;  /* 768, matches real FinalDW exactly */

    uintptr_t IN_PHYS   = FV_DDR_BASE + 0x00000;               /* 384*8*8 = 24576 B */
    uintptr_t WT_PHYS   = FV_DDR_BASE + 0x10000;                /* 768*9   = 6912  B */
    uintptr_t BIAS_PHYS = FV_DDR_BASE + 0x20000;                /* 768*4   = 3072  B */
    uintptr_t OUT_PHYS  = FV_DDR_BASE + 0x30000;                /* 768*4*4 = 12288 B */

    int8_t  *in_v   = (int8_t *)phys_to_virt(IN_PHYS);
    int8_t  *wt_v   = (int8_t *)phys_to_virt(WT_PHYS);
    int32_t *bias_v = (int32_t *)phys_to_virt(BIAS_PHYS);
    int8_t  *out_v  = (int8_t *)phys_to_virt(OUT_PHYS);

    for (int ch = 0; ch < CHin; ch++)
        for (int r = 0; r < Hin; r++)
            for (int c = 0; c < Win; c++)
                in_v[ch * Hin * Win + r * Win + c] = (int8_t)(ch + r * Win + c);

    memset(wt_v, 0, CHout * Kh * Kw);
    for (int ch = 0; ch < CHin; ch++) {
        int co0 = ch * fpg + 0, co1 = ch * fpg + 1;
        wt_v[co0 * Kh * Kw + 1 * Kw + 1] = 1;  /* center tap */
        wt_v[co1 * Kh * Kw + 0 * Kw + 0] = 1;  /* top-left tap */
    }
    for (int co = 0; co < CHout; co++) bias_v[co] = 0;
    memset(out_v, 0xAA, (size_t)CHout * Hout * Wout);  /* poison: leftover 0xAA = "never written" */

    fv_cache_flush(IN_PHYS, (size_t)CHin * Hin * Win);
    fv_cache_flush(WT_PHYS, (size_t)CHout * Kh * Kw);
    fv_cache_flush(BIAS_PHYS, (size_t)CHout * 4);

    fv_run_dwconv(IN_PHYS, WT_PHYS, BIAS_PHYS, OUT_PHYS,
                  CHin, Hin, Win, Kh, Kw, stride, stride, pad, pad,
                  fpg, ACT_NONE, /*out_shift=*/0);

    fv_cache_invalidate(OUT_PHYS, (size_t)CHout * Hout * Wout);

    int8_t *expect = malloc((size_t)CHout * Hout * Wout);
    for (int ch = 0; ch < CHin; ch++) {
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;
            for (int oh = 0; oh < Hout; oh++) {
                for (int ow = 0; ow < Wout; ow++) {
                    int ih, iw;
                    if (f == 0) { ih = oh * stride - pad + 1; iw = ow * stride - pad + 1; }
                    else        { ih = oh * stride - pad + 0; iw = ow * stride - pad + 0; }
                    int8_t v = (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                               ? (int8_t)(ch + ih * Win + iw) : 0;
                    expect[co * Hout * Wout + oh * Wout + ow] = v;
                }
            }
        }
    }

    printf("=== dwconv fpg=2 K=3 SCALED test: CHin=384 CHout=768 (matches real FinalDW exactly) ===\n");
    int mismatching_channels = 0, first_bad_co = -1, all_zero_channels = 0;
    for (int co = 0; co < CHout; co++) {
        int bad = 0, all_zero = 1;
        for (int i = 0; i < Hout * Wout; i++) {
            if (out_v[co * Hout * Wout + i] != expect[co * Hout * Wout + i]) bad = 1;
            if (out_v[co * Hout * Wout + i] != 0) all_zero = 0;
        }
        if (bad) {
            mismatching_channels++;
            if (first_bad_co < 0) first_bad_co = co;
        }
        if (all_zero) all_zero_channels++;
    }
    printf("mismatching channels: %d / %d\n", mismatching_channels, CHout);
    printf("all-zero-output channels: %d / %d\n", all_zero_channels, CHout);
    if (first_bad_co >= 0) {
        int co = first_bad_co;
        printf("--- first mismatching channel co=%d (ch=%d,f=%d) ---\n", co, co / fpg, co % fpg);
        for (int oh = 0; oh < Hout; oh++) {
            printf("  row%d  got:", oh);
            for (int ow = 0; ow < Wout; ow++) printf(" %4d", (int)out_v[co * Hout * Wout + oh * Wout + ow]);
            printf("   expect:");
            for (int ow = 0; ow < Wout; ow++) printf(" %4d", (int)expect[co * Hout * Wout + oh * Wout + ow]);
            printf("\n");
        }
        /* also show the LAST channel for comparison (does it degrade gradually or step-function?) */
        co = CHout - 1;
        printf("--- last channel co=%d (ch=%d,f=%d), for comparison ---\n", co, co / fpg, co % fpg);
        for (int oh = 0; oh < Hout; oh++) {
            printf("  row%d  got:", oh);
            for (int ow = 0; ow < Wout; ow++) printf(" %4d", (int)out_v[co * Hout * Wout + oh * Wout + ow]);
            printf("   expect:");
            for (int ow = 0; ow < Wout; ow++) printf(" %4d", (int)expect[co * Hout * Wout + oh * Wout + ow]);
            printf("\n");
        }
        /* find the FIRST channel index where it goes bad, by scanning ch order,
         * to see if there's a clean threshold (e.g. "breaks at ch>=256" style). */
        int first_bad_ch = -1;
        for (int ch = 0; ch < CHin && first_bad_ch < 0; ch++) {
            for (int f = 0; f < fpg; f++) {
                int c2 = ch * fpg + f;
                int bad2 = 0;
                for (int i = 0; i < Hout * Wout; i++)
                    if (out_v[c2 * Hout * Wout + i] != expect[c2 * Hout * Wout + i]) bad2 = 1;
                if (bad2) { first_bad_ch = ch; break; }
            }
        }
        printf("first bad input channel index (0-based): ch=%d of CHin=%d (co=%d)\n",
               first_bad_ch, CHin, first_bad_ch * fpg);
    }
    printf("=== %s ===\n", mismatching_channels == 0 ? "ALL MATCH" : "MISMATCHES FOUND");

    free(expect);
    munmap(dma_virt, DMA_SIZE);
    close(fd_dma);
    fv_driver_exit();
    return mismatching_channels != 0;
}
