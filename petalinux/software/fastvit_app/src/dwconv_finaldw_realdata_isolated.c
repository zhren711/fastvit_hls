/*================================================================
 * dwconv_finaldw_realdata_isolated.c — Phase 0.7 step 5 (2026-08-14)
 *
 * Redesigned poison test per ZHR-8 review comment: the original poison
 * test idea (memset the output, call the IP, read back, see if it's
 * still poison) has a hole -- memset() is an ARM write that can sit in
 * cache, so a readback showing "still poison" is ambiguous between
 * "the IP never wrote here" and "the IP wrote fresh data to DDR but we
 * read a stale cached copy". This version explicitly flushes the
 * poison write's cache lines are NOT flushed (poison must reach DDR is
 * not required -- see below) and, critically, INVALIDATES the output
 * region's cache after the call so the readback is forced to fetch
 * from DDR rather than any stale line.
 *
 * This is also a direct extension of Phase 0.6's "test 5"
 * (dwconv_fpg_k3_realaddr.c): same exact real ping/pong PHYSICAL
 * addresses, same real weight/bias bytes for layer 49 (FinalDW), same
 * out_shift=8 -- but with the REAL captured Stage4 activation
 * (accuracy_test_imgs/stage4_real_activation_0000.bin, dumped by
 * finaldw_shift_sweep.c mid full-network-run) as input instead of
 * all-zero. Test 5 used zero input and passed; this uses the exact
 * data that produces the {-1,0} bug in the full network, but as the
 * FIRST and ONLY FPGA op in a fresh process -- so it also answers
 * whether the bug needs ~50 prior ops to manifest (execution-history
 * dependent) or is deterministic given just (data, addresses).
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
#define DMA_SIZE     0x06000000UL

/* Real values for layer 49 (FinalDW), from weights_layout.h */
#define L49_W_OFFSET 2613408U
#define L49_B_OFFSET 3067488U
#define L49_OUT_SHIFT 8

#define POISON 0xA5  /* -91 signed -- distinct from 0, -1, and any plausible real conv output */

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
    const int n_out = CHout * Hout * Wout;

    uintptr_t IN_PHYS   = FV_FEAT_PING_BASE;              /* REAL ping address */
    uintptr_t OUT_PHYS  = FV_FEAT_PONG_BASE;              /* REAL pong address */
    uintptr_t WT_PHYS   = FV_DDR_BASE + L49_W_OFFSET;     /* REAL weight offset */
    uintptr_t BIAS_PHYS = FV_DDR_BASE + L49_B_OFFSET;     /* REAL bias offset */

    int8_t  *in_v   = (int8_t *)phys_to_virt(IN_PHYS);
    int8_t  *wt_v   = (int8_t *)phys_to_virt(WT_PHYS);
    int32_t *bias_v = (int32_t *)phys_to_virt(BIAS_PHYS);
    int8_t  *out_v  = (int8_t *)phys_to_virt(OUT_PHYS);

    FILE *fw = fopen("/home/root/weights_t8/layer_0049_dwconv_weight.bin", "rb");
    if (!fw) { perror("open weight file"); return 1; }
    size_t wr = fread(wt_v, 1, CHout * Kh * Kw, fw);
    fclose(fw);
    FILE *fb = fopen("/home/root/weights_t8/layer_0049_dwconv_bias.bin", "rb");
    if (!fb) { perror("open bias file"); return 1; }
    size_t br = fread(bias_v, 1, CHout * 4, fb);
    fclose(fb);
    FILE *fi = fopen("/home/root/stage4_real_activation_0000.bin", "rb");
    if (!fi) { perror("open real activation file"); return 1; }
    size_t ir = fread(in_v, 1, (size_t)CHin * Hin * Win, fi);
    fclose(fi);
    printf("loaded real weight (%zu B) + bias (%zu B) + REAL Stage4 activation (%zu B) for layer 49\n",
           wr, br, ir);

    /* Keep a private copy of the real activation for the reference
     * computation below -- wt_v/bias_v/in_v alias the SAME physical
     * memory the IP will read, which is fine (IP only reads), but we
     * want an independent snapshot untouched by anything that follows. */
    int8_t  *ref_in = malloc(CHin * Hin * Win);
    memcpy(ref_in, in_v, (size_t)CHin * Hin * Win);
    int8_t  *ref_wt = malloc((size_t)CHout * Kh * Kw);
    memcpy(ref_wt, wt_v, (size_t)CHout * Kh * Kw);
    int32_t *ref_bias = malloc(CHout * sizeof(int32_t));
    memcpy(ref_bias, bias_v, CHout * sizeof(int32_t));

    /* POISON the output region -- distinct, implausible value. */
    memset(out_v, POISON, (size_t)n_out);

    fv_cache_flush(IN_PHYS, (size_t)CHin * Hin * Win);
    fv_cache_flush(WT_PHYS, (size_t)CHout * Kh * Kw);
    fv_cache_flush(BIAS_PHYS, (size_t)CHout * 4);
    /* Also flush the poison write itself, so DDR truly holds POISON
     * before the IP runs (not required for correctness of the IP's
     * write, but keeps the "did DDR change" question unambiguous). */
    fv_cache_flush(OUT_PHYS, (size_t)n_out);

    printf("calling fv_run_dwconv at REAL ping/pong addresses: in=0x%lx wt=0x%lx bias=0x%lx out=0x%lx\n",
           (unsigned long)IN_PHYS, (unsigned long)WT_PHYS, (unsigned long)BIAS_PHYS, (unsigned long)OUT_PHYS);
    fv_run_dwconv(IN_PHYS, WT_PHYS, BIAS_PHYS, OUT_PHYS,
                  CHin, Hin, Win, Kh, Kw, stride, stride, pad, pad,
                  fpg, ACT_NONE, L49_OUT_SHIFT);

    /* Force the readback below to actually hit DDR, not a stale
     * cached copy of the POISON pattern (or of an earlier run's
     * result) sitting in ARM cache from before this call. */
    fv_cache_invalidate(OUT_PHYS, (size_t)n_out);

    /* -------- diagnostics -------- */
    int still_poison = 0;
    for (int i = 0; i < n_out; i++) if ((uint8_t)out_v[i] == POISON) still_poison++;

    int mn = 127, mx = -128; long sum = 0; int nz = 0;
    for (int i = 0; i < n_out; i++) {
        int v = out_v[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
        if (v != 0) nz++;
    }
    printf("\nreadback: min=%d max=%d mean=%.3f nonzero=%d/%d (%.1f%%)\n",
           mn, mx, (double)sum / n_out, nz, n_out, 100.0 * nz / n_out);
    printf("bytes still exactly POISON(0x%02X): %d / %d (%.1f%%)\n",
           POISON, still_poison, n_out, 100.0 * still_poison / n_out);

    /* -------- reference: same math as tools/verify_finaldw_math.py -------- */
    int mismatches = 0;
    int ref_mn = 127, ref_mx = -128; long ref_sum = 0; int ref_nz = 0;
    for (int co = 0; co < CHout; co++) {
        int ch = co / fpg;
        for (int oh = 0; oh < Hout; oh++) {
            for (int ow = 0; ow < Wout; ow++) {
                int64_t acc = ref_bias[co];
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ih = oh * stride - pad + kh;
                        int iw = ow * stride - pad + kw;
                        if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                            acc += (int64_t)ref_in[ch * Hin * Win + ih * Win + iw] *
                                   (int64_t)ref_wt[co * Kh * Kw + kh * Kw + kw];
                    }
                }
                int64_t s = acc >> L49_OUT_SHIFT;
                int8_t expect = (int8_t)(s > 127 ? 127 : s < -128 ? -128 : s);
                int idx = co * Hout * Wout + oh * Wout + ow;
                int got = out_v[idx];
                if (got != expect) mismatches++;
                if (expect < ref_mn) ref_mn = expect;
                if (expect > ref_mx) ref_mx = expect;
                ref_sum += expect;
                if (expect != 0) ref_nz++;
            }
        }
    }
    printf("reference (same math, real data): min=%d max=%d mean=%.3f nonzero=%d/%d (%.1f%%)\n",
           ref_mn, ref_mx, (double)ref_sum / n_out, ref_nz, n_out, 100.0 * ref_nz / n_out);
    printf("mismatches vs reference: %d / %d (%.1f%%)\n", mismatches, n_out, 100.0 * mismatches / n_out);

    printf("\n=== VERDICT ===\n");
    if (still_poison == n_out) {
        printf("IP NEVER WROTE to the output region -- readback is 100%% unchanged poison.\n");
    } else if (still_poison > 0) {
        printf("PARTIAL: %d/%d positions still poison, rest overwritten -- IP wrote SOME but not all positions.\n", still_poison, n_out);
    } else if (mismatches == 0) {
        printf("IP wrote fresh data and it EXACTLY MATCHES the real-data reference -- isolated single-shot call is HEALTHY (bug did NOT reproduce outside the 51-layer sequence).\n");
    } else {
        printf("IP wrote fresh (non-poison) data but it does NOT match the reference -- IP computed something, but wrong.\n");
    }

    free(ref_in); free(ref_wt); free(ref_bias);
    munmap(dma_virt, DMA_SIZE);
    close(fd_dma);
    fv_driver_exit();
    return 0;
}
