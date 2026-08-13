/*============================================================
 * tb_dwconv_worker.cpp -- standalone csim testbench for the line-buffer
 * dwconv rewrite (dwconv_linebuf/dwconv_worker.cpp).
 *
 * Ported from fastvit_ip_w8a4/fastvit_ip_tb.cpp's ref_dwconv/test_dwconv/
 * pack_bytes/unpack_bytes/check (see plan
 * C:\Users\zhren\.claude\plans\jaunty-questing-journal.md), calling
 * dwconv_worker() directly instead of routing through fastvit_ip's
 * op_code dispatch -- no dependency on the other 4 workers.
 *
 * Extends the original 7 stride=1/fpg=1 cases with a few stride=2 and
 * asymmetric-pad cases: the original ref_dwconv() hardcoded stride=1
 * (`ih = oh - pad + kh`, no stride multiply) and always used
 * pad = Kh/2, so neither the old testbench nor its reference model ever
 * exercised the general path -- this file's ref_dwconv() implements the
 * full formula (Hout/Wout depend on stride_h/stride_w/pad_h/pad_w) so
 * that claim can actually be checked here.
 *============================================================*/

#include "dwconv_worker.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_errors = 0;

static void pack_bytes(const act_t *src, pack_t *dst, int n) {
    int n_words = (n + 7) / 8;
    for (int w = 0; w < n_words; w++) {
        pack_t word = 0;
        for (int b = 0; b < 8; b++) {
            int idx = w * 8 + b;
            act_t v = (idx < n) ? src[idx] : (act_t)0;
            word.range(b*4+3, b*4) = v;
        }
        dst[w] = word;
    }
}

static void unpack_bytes(const pack_t *src, act_t *dst, int n) {
    int n_words = (n + 7) / 8;
    for (int w = 0; w < n_words; w++) {
        pack_t word = src[w];
        for (int b = 0; b < 8; b++) {
            int idx = w * 8 + b;
            if (idx < n) dst[idx] = (act_t)word.range(b*4+3, b*4);
        }
    }
}

static int check(const act_t *hw, const act_t *ref, int n, const char *name) {
    int err = 0;
    for (int i = 0; i < n; i++) {
        if (hw[i] != ref[i]) {
            err++;
            if (err <= 5)
                printf("  MISMATCH[%d]: hw=%d ref=%d\n", i, (int)hw[i], (int)ref[i]);
        }
    }
    printf("[%s] %s (%d points)\n", err ? "FAIL" : "PASS", name, n);
    g_errors += err;
    return err;
}

/* General reference model: full stride_h/stride_w/pad_h/pad_w support
 * (the ported original hardcoded stride=1 and pad=Kh/2 only). Must
 * reproduce dwconv_worker.cpp's dwconv_apply_act exactly. */
static void ref_dwconv(act_t *feat_in, wt_t *weight, acc_t *bias, act_t *feat_out,
                        int CH, int Hin, int Win, int Kh, int Kw,
                        int stride_h, int stride_w, int pad_h, int pad_w,
                        int fpg, int act_mode, int out_shift)
{
    int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    for (int ch = 0; ch < CH; ch++) {
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;
            for (int oh = 0; oh < Hout; oh++)
                for (int ow = 0; ow < Wout; ow++) {
                    acc_t sum = bias[co];
                    for (int kh = 0; kh < Kh; kh++)
                        for (int kw = 0; kw < Kw; kw++) {
                            int ih = oh*stride_h - pad_h + kh;
                            int iw = ow*stride_w - pad_w + kw;
                            if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                sum += (acc_t)feat_in[ch*Hin*Win + ih*Win + iw] *
                                       (acc_t)weight[co*Kh*Kw + kh*Kw + kw];
                        }
                    acc_t s = sum >> out_shift;
                    act_t v;  /* W8A4: symmetric 4-bit clamp -7..7 */
                    if      (s >  7) v =  7;
                    else if (s < -7) v = -7;
                    else             v = (act_t)s;
                    if (act_mode == ACT_RELU && v < 0) v = 0;
                    feat_out[co*Hout*Wout + oh*Wout + ow] = v;
                }
        }
    }
}

static int test_dwconv(int CH, int H, int W, int Kh, int Kw,
                        int stride_h, int stride_w, int pad_h, int pad_w,
                        int fpg, int act_mode, int out_shift, const char *name)
{
    int Hout = (H + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (W + 2*pad_w - Kw) / stride_w + 1;
    int CHout = CH * fpg;
    int n_in  = CH * H * W;
    int n_out = CHout * Hout * Wout;

    act_t *feat_in  = new act_t[n_in];
    wt_t  *weight   = new wt_t [CHout*Kh*Kw];
    acc_t *bias     = new acc_t[CHout];
    act_t *ref_out  = new act_t[n_out];
    act_t *hw_out   = new act_t[n_out];
    pack_t *in_a_p  = new pack_t[(n_in+7)/8];
    pack_t *out_p   = new pack_t[(n_out+7)/8];

    for (int i = 0; i < n_in;         i++) feat_in[i] = (act_t)(rand()%256-128);
    for (int i = 0; i < CHout*Kh*Kw;  i++) weight[i]  = (wt_t) (rand()%256-128);
    for (int i = 0; i < CHout;        i++) bias[i]    = (acc_t)(rand()%1024-512);

    pack_bytes(feat_in, in_a_p, n_in);
    dwconv_worker(in_a_p, weight, bias, out_p,
                  CH, H, W, Kh, Kw,
                  stride_h, stride_w, pad_h, pad_w,
                  fpg, act_mode, out_shift);
    unpack_bytes(out_p, hw_out, n_out);

    ref_dwconv(feat_in, weight, bias, ref_out, CH, H, W, Kh, Kw,
               stride_h, stride_w, pad_h, pad_w, fpg, act_mode, out_shift);

    int e = check(hw_out, ref_out, n_out, name);
    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] ref_out; delete[] hw_out; delete[] in_a_p; delete[] out_p;
    return e;
}

int main() {
    srand(42);

    printf("=== dwconv_linebuf csim ===\n");

    /* Original 7 cases, ported verbatim (stride=1, pad=Kh/2, fpg=1) --
     * the production/real-network subset. */
    test_dwconv(32, 56, 56, 3, 3, 1, 1, 1, 1, 1, ACT_RELU, 8, "DW3_s1_CH32");
    test_dwconv(48, 64, 64, 3, 3, 1, 1, 1, 1, 1, ACT_NONE, 7, "DW3_FastVIT_S1");
    test_dwconv(16, 28, 28, 7, 7, 1, 1, 3, 3, 1, ACT_NONE, 7, "DW7_s1_CH16");
    test_dwconv(48, 16, 16, 7, 7, 1, 1, 3, 3, 1, ACT_NONE, 7, "DW7_FastVIT_S3");
    test_dwconv( 8,  4,  4, 3, 3, 1, 1, 1, 1, 1, ACT_RELU, 6, "DW3_small");
    test_dwconv( 3,  8,  8, 3, 3, 1, 1, 1, 1, 1, ACT_NONE, 6, "DW3_CH3_odd");
    test_dwconv( 5,  8,  8, 7, 7, 1, 1, 3, 3, 1, ACT_NONE, 6, "DW7_CH5_odd");

    /* New: stride=2 cases (never exercised by the old tile-based
     * testbench -- its ref model hardcoded stride=1 math). Dimensions
     * chosen so Hin*Win and Hout*Wout are both multiples of 8 -- the
     * 8-nibbles/word packing granularity that the >>3 word-count
     * formula (inherited unchanged from the original design, see
     * hw_in_words/hw_out_words below) has always assumed; every real
     * FastVIT layer satisfies this since its dims are powers of two,
     * so this is a pre-existing packing-scheme constraint, not
     * something to work around in the compute core itself. Hin=15
     * (non-power-of-2, deliberately not "nice") still exercises a
     * genuinely odd row count on top of stride=2. */
    test_dwconv(16, 32, 32, 3, 3, 2, 2, 1, 1, 1, ACT_NONE, 7, "DW3_stride2_CH16");
    test_dwconv( 8, 15, 16, 3, 3, 2, 2, 1, 1, 1, ACT_RELU, 6, "DW3_stride2_odd_dim");
    test_dwconv( 4, 20, 32, 7, 7, 2, 2, 3, 3, 1, ACT_NONE, 7, "DW7_stride2_CH4");

    /* New: asymmetric / non-K/2 padding (VALID-ish, pad=0) and
     * asymmetric stride_h != stride_w -- also never exercised before.
     * Same packing-granularity constraint as above (Win=14/Wout=12
     * chosen so Hin*Win=112 and Hout*Wout=72 are both /8 exact). */
    test_dwconv( 4,  8, 14, 3, 3, 1, 1, 0, 0, 1, ACT_NONE, 6, "DW3_pad0_VALID");
    test_dwconv( 4, 16, 12, 3, 3, 2, 1, 1, 0, 1, ACT_RELU, 6, "DW3_asym_stride_pad");

    /* New: fpg>1 (expand factor) -- also unexercised by the old
     * testbench (always passed fpg=1); dead on the real FPGA workload
     * per project research but must stay functionally correct since the
     * signature still accepts it. */
    test_dwconv( 4,  8,  8, 3, 3, 1, 1, 1, 1, 3, ACT_NONE, 6, "DW3_fpg3");

    printf("=== TOTAL ERRORS: %d ===\n", g_errors);
    if (g_errors == 0) printf("ALL TESTS PASSED\n");
    return g_errors ? 1 : 0;
}
