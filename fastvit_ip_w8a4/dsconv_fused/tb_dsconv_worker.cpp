/*============================================================
 * tb_dsconv_worker.cpp -- standalone csim testbench for dsconv_worker.
 *
 * Reference model: ref_dwconv chained directly into ref_pwconv, copied
 * verbatim (types adjusted to this header's self-contained typedefs)
 * from fastvit_ip_w8a4/fastvit_ip_tb.cpp:132 and :192 -- both already
 * assume stride=1/pad=Kh//2 ("same" padding), matching this fusion's
 * only real target link (RepMixer's trailing DW7 -> PW1) exactly, so
 * no generalization was needed.
 *
 * Checks BOTH DUT outputs against reference: dw_feat_out (the raw DW
 * copy materialized for the block's later residual Add) against
 * ref_dwconv's output directly, and pw_feat_out (the fused PW1 result)
 * against ref_pwconv fed by ref_dwconv's output -- this validates the
 * hard correctness constraint from dsconv_worker.cpp's header comment
 * (dw_feat_out must be bit-exact, not just "close enough to feed PW"),
 * not just the final fused result.
 *
 * Test shapes: the 4 representative real RepMixer blocks from the plan
 * (C:\Users\zhren\.claude\plans\peppy-gathering-ripple.md) -- S1B0
 * (smallest C, largest spatial), S2B0 (a pruned-CHout block), S3B0
 * (Npatches=1, "whole image as one patch" case), S4B1 (largest C,
 * smallest spatial, also Npatches=1, a pruned-CHout block) -- chosen
 * because the DRAM-traffic tradeoff (see plan) is stage-dependent and
 * only shows up by testing all four, and S3/S4 exercise the
 * Npatches=1 degenerate case explicitly.
 *============================================================*/

#include "dsconv_worker.h"
#include <cstdio>
#include <cstdlib>

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

/* Copied verbatim from fastvit_ip_w8a4/fastvit_ip_tb.cpp:132 (types
 * adjusted to this header's self-contained typedefs). */
static void ref_dwconv(act_t *feat_in, wt_t *weight, acc_t *bias, act_t *feat_out,
                        int CH, int Hin, int Win, int Kh, int Kw, int pad,
                        int act_mode, int out_shift)
{
    for (int ch = 0; ch < CH; ch++)
        for (int oh = 0; oh < Hin; oh++)
            for (int ow = 0; ow < Win; ow++) {
                acc_t sum = bias[ch];
                for (int kh = 0; kh < Kh; kh++)
                    for (int kw = 0; kw < Kw; kw++) {
                        int ih = oh - pad + kh;
                        int iw = ow - pad + kw;
                        if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                            sum += (acc_t)feat_in[ch*Hin*Win + ih*Win + iw] *
                                   (acc_t)weight[ch*Kh*Kw + kh*Kw + kw];
                    }
                acc_t s = sum >> out_shift;
                act_t v;
                if      (s >  7) v =  7;
                else if (s < -7) v = -7;
                else             v = (act_t)s;
                if (act_mode == ACT_RELU && v < 0) v = 0;
                feat_out[ch*Hin*Win + oh*Win + ow] = v;
            }
}

/* Copied verbatim from fastvit_ip_w8a4/fastvit_ip_tb.cpp:192. */
static void ref_pwconv(act_t *feat_in, wt_t *weight, acc_t *bias, act_t *feat_out,
                        int CHin, int H, int W, int CHout, int act_mode, int out_shift)
{
    int spatial = H * W;
    for (int co = 0; co < CHout; co++)
        for (int s = 0; s < spatial; s++) {
            acc_t sum = bias[co];
            for (int ci = 0; ci < CHin; ci++)
                sum += (acc_t)feat_in[ci*spatial+s] * (acc_t)weight[co*CHin+ci];
            acc_t shifted = sum >> out_shift;
            act_t v;
            if      (shifted >  7) v =  7;
            else if (shifted < -7) v = -7;
            else                   v = (act_t)shifted;
            if (act_mode == ACT_RELU && v < 0) v = 0;
            feat_out[co*spatial+s] = v;
        }
}

static int run_test(int CHin, int H, int W, int CHout, const char *name)
{
    printf("--- %s: CHin=%d H=W=%d CHout=%d ---\n", name, CHin, H, CHout);
    const int Kh = 7, Kw = 7, pad = 3;
    int n_dw_in  = CHin * H * W;
    int n_dw_out = CHin * H * W;   /* stride=1, "same" pad -> Hout=Hin,Wout=Win */
    int n_pw_out = CHout * H * W;

    act_t *dw_feat_in  = new act_t[n_dw_in];
    wt_t  *dw_weight   = new wt_t [CHin * Kh * Kw];
    acc_t *dw_bias     = new acc_t[CHin];
    wt_t  *pw_weight   = new wt_t [CHout * CHin];
    acc_t *pw_bias     = new acc_t[CHout];

    for (int i = 0; i < n_dw_in;        i++) dw_feat_in[i] = (act_t)(rand()%256-128);
    for (int i = 0; i < CHin*Kh*Kw;     i++) dw_weight[i]  = (wt_t) (rand()%256-128);
    for (int i = 0; i < CHin;           i++) dw_bias[i]    = (acc_t)(rand()%1024-512);
    for (int i = 0; i < CHout*CHin;     i++) pw_weight[i]  = (wt_t) (rand()%256-128);
    for (int i = 0; i < CHout;          i++) pw_bias[i]    = (acc_t)(rand()%1024-512);

    /* ---- reference: ref_dwconv chained into ref_pwconv ---- */
    act_t *ref_dw_out = new act_t[n_dw_out];
    act_t *ref_pw_out = new act_t[n_pw_out];
    ref_dwconv(dw_feat_in, dw_weight, dw_bias, ref_dw_out, CHin, H, W, Kh, Kw, pad, ACT_NONE, 0);
    ref_pwconv(ref_dw_out, pw_weight, pw_bias, ref_pw_out, CHin, H, W, CHout, ACT_NONE, 0);

    /* ---- DUT ---- */
    pack_t *dw_in_p   = new pack_t[(n_dw_in  + 7) / 8];
    pack_t *dw_out_p  = new pack_t[(n_dw_out + 7) / 8];
    pack_t *pw_out_p  = new pack_t[(n_pw_out + 7) / 8];
    pack_bytes(dw_feat_in, dw_in_p, n_dw_in);

    dsconv_worker(dw_in_p, dw_weight, dw_bias, dw_out_p,
                  pw_weight, pw_bias, pw_out_p,
                  CHin, H, W, Kh, Kw, pad, pad, CHout,
                  ACT_NONE, 0, ACT_NONE, 0);

    act_t *hw_dw_out = new act_t[n_dw_out];
    act_t *hw_pw_out = new act_t[n_pw_out];
    unpack_bytes(dw_out_p, hw_dw_out, n_dw_out);
    unpack_bytes(pw_out_p, hw_pw_out, n_pw_out);

    char buf[128];
    snprintf(buf, sizeof(buf), "%s (DW residual copy)", name);
    int e1 = check(hw_dw_out, ref_dw_out, n_dw_out, buf);
    snprintf(buf, sizeof(buf), "%s (fused PW1 result)", name);
    int e2 = check(hw_pw_out, ref_pw_out, n_pw_out, buf);

    delete[] dw_feat_in; delete[] dw_weight; delete[] dw_bias;
    delete[] pw_weight; delete[] pw_bias;
    delete[] ref_dw_out; delete[] ref_pw_out;
    delete[] dw_in_p; delete[] dw_out_p; delete[] pw_out_p;
    delete[] hw_dw_out; delete[] hw_pw_out;
    return e1 + e2;
}

int main()
{
    srand(42);

    /* S1B0: smallest C, largest spatial (Npatches=16) */
    run_test(48, 64, 64, 144, "S1B0");
    /* S2B0: pruned CHout (Npatches=4) */
    run_test(96, 32, 32, 240, "S2B0_pruned");
    /* S3B0: Npatches=1 (whole image as one patch), full CHout */
    run_test(192, 16, 16, 576, "S3B0");
    /* S4B1: largest C, smallest spatial, Npatches=1, pruned CHout */
    run_test(384, 8, 8, 960, "S4B1_pruned");

    /* SYNTH_GAP: synthetic, not a real FastVIT-T8 shape -- constructed
     * specifically to hit the one gap PW_WT_CACHE_MAX's caching doesn't
     * cover (CHin*CHout=128*384=49152 > PW_WT_CACHE_MAX=32768, so
     * pw_wt_cache can't hold it) while ALSO having Npatches>1
     * (H=W=32 -> 2x2=4 patches, so the per-patch DMA fallback isn't
     * free either), a combination no real block in this network
     * produces (see dsconv_worker.h's PATCH_GROUP comment). Exists to
     * exercise PATCH_GROUP's grouped-weight-tile-reuse path, which is
     * otherwise dead code against the other four shapes above. */
    run_test(128, 32, 32, 384, "SYNTH_GAP_cache_miss_multipatch");

    printf("=== TOTAL ERRORS: %d ===\n", g_errors);
    if (g_errors == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("TESTS FAILED\n");
        return 1;
    }
}
