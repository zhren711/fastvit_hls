/*============================================================
 * dwconv_finaldw_realdata_tb.cpp — Phase 0.7 step 4 (2026-08-14)
 *
 * Calls dwconv_worker() (fastvit_ip/dwconv_worker.cpp, direct
 * pass-through from fastvit_ip's OP_DWCONV dispatch -- see
 * fastvit_ip.cpp case OP_DWCONV) in HLS C-simulation, using the
 * REAL captured FinalDW inputs from the live board:
 *   - weights_t8/layer_0049_dwconv_{weight,bias}.bin (md5-verified
 *     identical to what's actually loaded on the board)
 *   - accuracy_test_imgs/stage4_real_activation_0000.bin (the exact
 *     real Stage4 output byte-dumped from finaldw_shift_sweep.c
 *     immediately before the real board's FinalDW call)
 *
 * Real hardware, for this exact data (Phase 0.7 step 2, Linear
 * ZHR-8): output frozen at min=-1 max=0 mean=-0.829,
 * nonzero=10192/12288 (82.9%), IDENTICAL across out_shift in
 * {8,6,4,2,0}.
 *
 * Software replica of the documented math (Phase 0.7 step 3,
 * tools/verify_finaldw_math.py): healthy full-range output,
 * min=-128 max=127 mean=-1.837 nonzero=12259/12288 (99.8%) at
 * out_shift=8.
 *
 * This test asks: does dwconv_worker.cpp's OWN C++ SOURCE, run via
 * HLS csim (same compute description Vitis HLS synthesizes into
 * RTL) on this exact real data, match the real hardware's degenerate
 * output, or the healthy reference? If it matches hardware, the bug
 * is algorithmic and in scope for pure C++ debugging. If it matches
 * the healthy reference instead, the bug is downstream of the C++
 * description (HLS scheduling/RTL codegen), a much harder class.
 *============================================================*/

#include "fastvit_ip.h"
#include "dwconv_worker.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static void pack_bytes(const act_t *src, pack_t *dst, int n) {
    int n_words = (n + 3) / 4;
    for (int w = 0; w < n_words; w++) {
        pack_t word = 0;
        for (int b = 0; b < 4; b++) {
            int idx = w * 4 + b;
            act_t v = (idx < n) ? src[idx] : (act_t)0;
            word.range(b*8+7, b*8) = v;
        }
        dst[w] = word;
    }
}

static void unpack_bytes(const pack_t *src, act_t *dst, int n) {
    int n_words = (n + 3) / 4;
    for (int w = 0; w < n_words; w++) {
        pack_t word = src[w];
        for (int b = 0; b < 4; b++) {
            int idx = w * 4 + b;
            if (idx < n) dst[idx] = (act_t)word.range(b*8+7, b*8);
        }
    }
}

/* Same math as tools/verify_finaldw_math.py, in HLS types. */
static void ref_dwconv_general(
    const act_t *feat_in, const wt_t *weight, const acc_t *bias, act_t *feat_out,
    int CHin, int Hin, int Win, int Kh, int Kw,
    int stride_h, int stride_w, int pad_h, int pad_w, int fpg,
    int act_mode, int out_shift)
{
    int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    for (int ch = 0; ch < CHin; ch++) {
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;
            for (int oh = 0; oh < Hout; oh++) {
                for (int ow = 0; ow < Wout; ow++) {
                    acc_t sum = bias[co];
                    for (int kh = 0; kh < Kh; kh++) {
                        for (int kw = 0; kw < Kw; kw++) {
                            int ih = oh*stride_h - pad_h + kh;
                            int iw = ow*stride_w - pad_w + kw;
                            if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                sum += (acc_t)feat_in[ch*Hin*Win + ih*Win + iw] *
                                       (acc_t)weight[co*Kh*Kw + kh*Kw + kw];
                        }
                    }
                    acc_t s = sum >> out_shift;
                    act_t v;
                    if      (s >  127) v =  127;
                    else if (s < -128) v = -128;
                    else               v = (act_t)s;
                    if (act_mode == ACT_RELU && v < 0) v = 0;
                    feat_out[co*Hout*Wout + oh*Wout + ow] = v;
                }
            }
        }
    }
}

static void stats(const act_t *a, int n, int *mn, int *mx, double *mean, int *nz) {
    long sum = 0; *nz = 0; *mn = 127; *mx = -128;
    for (int i = 0; i < n; i++) {
        int v = (int)a[i];
        if (v < *mn) *mn = v;
        if (v > *mx) *mx = v;
        sum += v;
        if (v != 0) (*nz)++;
    }
    *mean = (double)sum / n;
}

static int load_file(const char *path, char *buf, long expect_bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("ERROR: cannot open %s\n", path); return -1; }
    long n = (long)fread(buf, 1, expect_bytes, f);
    fclose(f);
    if (n != expect_bytes) {
        printf("ERROR: %s read %ld bytes, expected %ld\n", path, n, expect_bytes);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *base = (argc > 1) ? argv[1] : "..";  /* repo root, override via argv[1] */

    const int CHin = 384, Hin = 8, Win = 8, Kh = 3, Kw = 3;
    const int stride_h = 2, stride_w = 2, pad_h = 1, pad_w = 1, fpg = 2;
    const int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    const int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    const int CHout = CHin * fpg;
    const int n_in  = CHin * Hin * Win;
    const int n_out = CHout * Hout * Wout;

    printf("=== dwconv_worker csim on REAL FinalDW data ===\n");
    printf("CHin=%d Hin=%d Win=%d Kh=%d Kw=%d stride=%d pad=%d fpg=%d -> CHout=%d Hout=%d Wout=%d\n",
           CHin, Hin, Win, Kh, Kw, stride_h, pad_h, fpg, CHout, Hout, Wout);

    act_t *feat_in  = new act_t[n_in];
    wt_t  *weight   = new wt_t[CHout * Kh * Kw];
    acc_t *bias     = new acc_t[CHout];
    act_t *hw_out   = new act_t[n_out];
    act_t *ref_out  = new act_t[n_out];
    pack_t *in_a_p  = new pack_t[(n_in + 3) / 4];
    pack_t *out_p   = new pack_t[(n_out + 3) / 4];

    char path[512];
    snprintf(path, sizeof(path), "%s/accuracy_test_imgs/stage4_real_activation_0000.bin", base);
    {
        int8_t raw[384*8*8];
        if (load_file(path, (char*)raw, sizeof(raw)) != 0) return 1;
        for (int i = 0; i < n_in; i++) feat_in[i] = (act_t)raw[i];
    }
    snprintf(path, sizeof(path), "%s/weights_t8/layer_0049_dwconv_weight.bin", base);
    {
        int8_t raw[768*3*3];
        if (load_file(path, (char*)raw, sizeof(raw)) != 0) return 1;
        for (int i = 0; i < CHout*Kh*Kw; i++) weight[i] = (wt_t)raw[i];
    }
    snprintf(path, sizeof(path), "%s/weights_t8/layer_0049_dwconv_bias.bin", base);
    {
        int32_t raw[768];
        if (load_file(path, (char*)raw, sizeof(raw)) != 0) return 1;
        for (int i = 0; i < CHout; i++) bias[i] = (acc_t)raw[i];
    }

    pack_bytes(feat_in, in_a_p, n_in);

    for (int trial = 0; trial < 5; trial++) {
        int out_shift = (int[]){8,6,4,2,0}[trial];

        dwconv_worker(in_a_p, weight, bias, out_p,
                      CHin, Hin, Win, Kh, Kw,
                      stride_h, stride_w, pad_h, pad_w,
                      fpg, ACT_NONE, out_shift);
        unpack_bytes(out_p, hw_out, n_out);

        ref_dwconv_general(feat_in, weight, bias, ref_out,
                            CHin, Hin, Win, Kh, Kw,
                            stride_h, stride_w, pad_h, pad_w, fpg,
                            ACT_NONE, out_shift);

        int hmn,hmx,hnz, rmn,rmx,rnz; double hmean,rmean;
        stats(hw_out, n_out, &hmn,&hmx,&hmean,&hnz);
        stats(ref_out, n_out, &rmn,&rmx,&rmean,&rnz);

        int mismatches = 0;
        for (int i = 0; i < n_out; i++) if (hw_out[i] != ref_out[i]) mismatches++;

        printf("\n--- out_shift=%d ---\n", out_shift);
        printf("  dwconv_worker (csim) : min=%4d max=%4d mean=%8.3f nonzero=%5d/%d (%.1f%%)\n",
               hmn, hmx, hmean, hnz, n_out, 100.0*hnz/n_out);
        printf("  ref_dwconv_general   : min=%4d max=%4d mean=%8.3f nonzero=%5d/%d (%.1f%%)\n",
               rmn, rmx, rmean, rnz, n_out, 100.0*rnz/n_out);
        printf("  mismatches: %d / %d (%.1f%%)\n", mismatches, n_out, 100.0*mismatches/n_out);
    }

    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] hw_out; delete[] ref_out; delete[] in_a_p; delete[] out_p;
    return 0;
}
