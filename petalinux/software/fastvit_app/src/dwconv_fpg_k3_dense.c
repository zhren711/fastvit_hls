/*================================================================
 * dwconv_fpg_k3_dense.c — Phase 0.6 third follow-up.
 *
 * Ruled out so far (all passed cleanly): fpg=2+K=3 interaction at
 * tiny scale, at full real scale (CHin=384/CHout=768), and after
 * warming up through every other op_code first. All of those tests
 * used a SPARSE 1-tap weight (rest zero) so only ONE of the 9
 * (kh,kw) MACs per output pixel was ever actually non-zero -- never
 * exercising real 9-tap accumulation. This test uses a DENSE all-1s
 * 3x3 weight for every output channel, so all 9 taps genuinely
 * accumulate (with zero-padding at the borders same as before).
 * Expected output = sum of the valid (non-padded) input pixels in
 * each output position's 3x3 receptive field -- still hand-computable.
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

    /* Small input values (0..3ish) so a 9-tap sum can't overflow int8
     * even before any shift -- keeps the expected-value math trivial
     * while still exercising real multi-tap accumulation. */
    for (int ch = 0; ch < CHin; ch++)
        for (int r = 0; r < Hin; r++)
            for (int c = 0; c < Win; c++)
                in_v[ch * Hin * Win + r * Win + c] = (int8_t)((r + c) % 4);

    /* DENSE: all 9 taps = 1 for every output channel -> real accumulation */
    for (int i = 0; i < CHout * Kh * Kw; i++) wt_v[i] = 1;
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
                    int32_t acc = 0;
                    for (int kh = 0; kh < Kh; kh++) {
                        for (int kw = 0; kw < Kw; kw++) {
                            int ih = oh * stride - pad + kh;
                            int iw = ow * stride - pad + kw;
                            if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                acc += (int8_t)((ih + iw) % 4);  /* weight=1, so just sum input */
                        }
                    }
                    /* out_shift=0, so acc IS the output, clamped to int8 */
                    if (acc > 127) acc = 127;
                    if (acc < -128) acc = -128;
                    expect[co * Hout * Wout + oh * Wout + ow] = (int8_t)acc;
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
