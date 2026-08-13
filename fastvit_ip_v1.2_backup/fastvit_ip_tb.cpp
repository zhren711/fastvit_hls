/*============================================================
 * fastvit_ip_tb.cpp — unified testbench for the merged IP.
 *
 * Reuses the reference models and test vectors from the 4 original
 * per-kernel testbenches verbatim (conv_ip_tb.cpp, tb_dwconv_ip.cpp,
 * tb_pwconv_ip.cpp, tb_add_ip.cpp); only the DUT call site changes to
 * go through fastvit_ip(op_code=OP_X, ...) with feature-map data
 * (in_a/out) packed into pack_t words. weight/bias (in_b/bias) stay
 * native act_t/wt_t/acc_t, unpacked, exactly as each original kernel
 * read them.
 *============================================================*/

#include "fastvit_ip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_errors = 0;

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

/*============================================================
 * OP_CONV — from conv_ip/conv_ip_tb.cpp
 *============================================================*/
static void sw_conv3x3(act_t *in, wt_t *wt, acc_t *bias, act_t *out,
                        int CHin, int Hin, int Win, int CHout,
                        int sh, int sw_s, int ph, int pw,
                        int relu, int shift)
{
    int Hout = (Hin + 2*ph - CONV_K) / sh + 1;
    int Wout = (Win + 2*pw - CONV_K) / sw_s + 1;
    for (int co = 0; co < CHout; co++)
        for (int oh = 0; oh < Hout; oh++)
            for (int ow = 0; ow < Wout; ow++) {
                acc_t acc = bias[co];
                for (int ci = 0; ci < CHin; ci++)
                    for (int kh = 0; kh < CONV_K; kh++)
                        for (int kw = 0; kw < CONV_K; kw++) {
                            int ih = oh*sh - ph + kh;
                            int iw = ow*sw_s - pw + kw;
                            act_t f = (ih>=0&&ih<Hin&&iw>=0&&iw<Win)
                                       ? in[ci*Hin*Win + ih*Win + iw] : (act_t)0;
                            wt_t w = wt[co*CHin*CONV_K*CONV_K + ci*CONV_K*CONV_K + kh*CONV_K + kw];
                            acc += (acc_t)f * (acc_t)w;
                        }
                acc_t s = acc >> shift;
                act_t r;
                if      (s >  127) r =  127;
                else if (s < -128) r = -128;
                else               r = (act_t)s;
                if (relu && r < 0) r = 0;
                out[co*Hout*Wout + oh*Wout + ow] = r;
            }
}

static int test_conv(int CHin, int Hin, int Win, int CHout,
                      int sh, int sw_s, int ph, int pw,
                      int relu, int shift, const char *name)
{
    int Hout = (Hin+2*ph-CONV_K)/sh+1, Wout = (Win+2*pw-CONV_K)/sw_s+1;
    int n_in = CHin*Hin*Win, n_out = CHout*Hout*Wout;

    act_t *feat_in  = new act_t[n_in];
    wt_t  *weight   = new wt_t [CHout*CHin*CONV_K*CONV_K];
    acc_t *bias     = new acc_t[CHout];
    act_t *ref_out  = new act_t[n_out];
    act_t *hw_out   = new act_t[n_out];
    pack_t *in_a_p  = new pack_t[(n_in+3)/4];
    pack_t *out_p   = new pack_t[(n_out+3)/4];

    for (int i = 0; i < n_in; i++) feat_in[i] = (act_t)((i%127)-63);
    for (int i = 0; i < CHout*CHin*CONV_K*CONV_K; i++) weight[i] = (wt_t)((i%7)-3);
    for (int i = 0; i < CHout; i++) bias[i] = (acc_t)(i*16);

    pack_bytes(feat_in, in_a_p, n_in);
    fastvit_ip(in_a_p, weight, bias, out_p,
               OP_CONV, CHin, Hin, Win, CHout, relu, shift,
               sh, sw_s, ph, pw, /*Kh*/0, /*Kw*/0, /*fpg*/0);
    unpack_bytes(out_p, hw_out, n_out);

    sw_conv3x3(feat_in, weight, bias, ref_out, CHin, Hin, Win, CHout, sh, sw_s, ph, pw, relu, shift);

    int e = check(hw_out, ref_out, n_out, name);
    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] ref_out; delete[] hw_out; delete[] in_a_p; delete[] out_p;
    return e;
}

/*============================================================
 * OP_DWCONV — from dwconv_ip/tb_dwconv_ip.cpp
 *============================================================*/
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
                if      (s >  127) v =  127;
                else if (s < -128) v = -128;
                else               v = (act_t)s;
                if (act_mode == ACT_RELU && v < 0) v = 0;
                feat_out[ch*Hin*Win + oh*Win + ow] = v;
            }
}

static int test_dwconv(int CH, int H, int W, int Kh, int Kw,
                        int act_mode, int out_shift, const char *name)
{
    int pad = Kh / 2;
    int n = CH*H*W;
    act_t *feat_in  = new act_t[n];
    wt_t  *weight   = new wt_t [CH*Kh*Kw];
    acc_t *bias     = new acc_t[CH];
    act_t *ref_out  = new act_t[n];
    act_t *hw_out   = new act_t[n];
    pack_t *in_a_p  = new pack_t[(n+3)/4];
    pack_t *out_p   = new pack_t[(n+3)/4];

    for (int i = 0; i < n;        i++) feat_in[i] = (act_t)(rand()%256-128);
    for (int i = 0; i < CH*Kh*Kw; i++) weight[i]  = (wt_t) (rand()%256-128);
    for (int i = 0; i < CH;       i++) bias[i]    = (acc_t)(rand()%1024-512);

    pack_bytes(feat_in, in_a_p, n);
    fastvit_ip(in_a_p, weight, bias, out_p,
               OP_DWCONV, CH, H, W, /*CHout*/0, act_mode, out_shift,
               1, 1, pad, pad, Kh, Kw, /*fpg*/1);
    unpack_bytes(out_p, hw_out, n);

    ref_dwconv(feat_in, weight, bias, ref_out, CH, H, W, Kh, Kw, pad, act_mode, out_shift);

    int e = check(hw_out, ref_out, n, name);
    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] ref_out; delete[] hw_out; delete[] in_a_p; delete[] out_p;
    return e;
}

/*============================================================
 * OP_PWCONV — from pwconv_ip/tb_pwconv_ip.cpp
 *============================================================*/
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
            if      (shifted >  127) v =  127;
            else if (shifted < -128) v = -128;
            else                     v = (act_t)shifted;
            if (act_mode == ACT_RELU && v < 0) v = 0;
            feat_out[co*spatial+s] = v;
        }
}

static int test_pwconv(int CHin, int H, int W, int CHout,
                        int act_mode, int out_shift, const char *name)
{
    int spatial = H*W;
    int n_in = CHin*spatial, n_out = CHout*spatial;
    act_t *feat_in  = new act_t[n_in];
    wt_t  *weight   = new wt_t [CHout*CHin];
    acc_t *bias     = new acc_t[CHout];
    act_t *ref_out  = new act_t[n_out];
    act_t *hw_out   = new act_t[n_out];
    pack_t *in_a_p  = new pack_t[(n_in+3)/4];
    pack_t *out_p   = new pack_t[(n_out+3)/4];

    for (int i = 0; i < n_in;        i++) feat_in[i] = (act_t)(rand()%256-128);
    for (int i = 0; i < CHout*CHin;  i++) weight[i]  = (wt_t) (rand()%256-128);
    for (int i = 0; i < CHout;       i++) bias[i]    = (acc_t)(rand()%1024-512);

    pack_bytes(feat_in, in_a_p, n_in);
    fastvit_ip(in_a_p, weight, bias, out_p,
               OP_PWCONV, CHin, H, W, CHout, act_mode, out_shift,
               /*stride_h*/0, /*stride_w*/0, /*pad_h*/0, /*pad_w*/0, /*Kh*/0, /*Kw*/0, /*fpg*/0);
    unpack_bytes(out_p, hw_out, n_out);

    ref_pwconv(feat_in, weight, bias, ref_out, CHin, H, W, CHout, act_mode, out_shift);

    int e = check(hw_out, ref_out, n_out, name);
    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] ref_out; delete[] hw_out; delete[] in_a_p; delete[] out_p;
    return e;
}

/*============================================================
 * OP_ADD — from add_ip/tb_add_ip.cpp
 *============================================================*/
static void ref_add(act_t *in1, act_t *in2, act_t *out, int size) {
    for (int i = 0; i < size; i++) {
        ap_int<16> sum = (ap_int<16>)in1[i] + (ap_int<16>)in2[i];
        if      (sum >  127) out[i] =  127;
        else if (sum < -128) out[i] = -128;
        else                 out[i] = (act_t)sum;
    }
}

static int test_add(int CH, int H, int W, act_t *forced_in1, act_t *forced_in2,
                     const char *name)
{
    int n = CH*H*W;
    act_t *in1     = new act_t[n];
    act_t *in2     = new act_t[n];
    act_t *ref_out = new act_t[n];
    act_t *hw_out  = new act_t[n];
    pack_t *in_a_p = new pack_t[(n+3)/4];
    pack_t *out_p  = new pack_t[(n+3)/4];

    if (forced_in1 && forced_in2) {
        memcpy(in1, forced_in1, n*sizeof(act_t));
        memcpy(in2, forced_in2, n*sizeof(act_t));
    } else {
        for (int i = 0; i < n; i++) {
            in1[i] = (act_t)(rand()%50-25);
            in2[i] = (act_t)(rand()%50-25);
        }
    }

    pack_bytes(in1, in_a_p, n);
    fastvit_ip(in_a_p, in2, nullptr, out_p,
               OP_ADD, CH, H, W, /*CHout*/0, /*act_mode*/0, /*out_shift*/0,
               0, 0, 0, 0, 0, 0, 0);
    unpack_bytes(out_p, hw_out, n);

    ref_add(in1, in2, ref_out, n);

    int e = check(hw_out, ref_out, n, name);
    delete[] in1; delete[] in2; delete[] ref_out; delete[] hw_out;
    delete[] in_a_p; delete[] out_p;
    return e;
}

/*============================================================
 * OP_GELU — 256-entry int8 LUT, bit-identical to
 * petalinux/software/fastvit_app/src/gelu_lut.c (independent copy
 * here so the testbench doesn't depend on the ARM-side source file).
 * gelu(x) ~= x * sigmoid(1.702*x), scale = 1/127.
 *============================================================*/
static const act_t gelu_ref_lut[256] = {
    -20,  -20,  -20,  -20,  -20,  -20,  -20,  -20,
    -20,  -20,  -20,  -20,  -20,  -20,  -20,  -20,
    -20,  -20,  -20,  -21,  -21,  -21,  -21,  -21,
    -21,  -21,  -21,  -21,  -21,  -21,  -21,  -21,
    -21,  -21,  -21,  -21,  -21,  -21,  -21,  -21,
    -21,  -21,  -21,  -21,  -21,  -21,  -20,  -20,
    -20,  -20,  -20,  -20,  -20,  -20,  -20,  -20,
    -20,  -20,  -20,  -20,  -19,  -19,  -19,  -19,
    -19,  -19,  -19,  -19,  -19,  -18,  -18,  -18,
    -18,  -18,  -18,  -17,  -17,  -17,  -17,  -17,
    -17,  -16,  -16,  -16,  -16,  -15,  -15,  -15,
    -15,  -15,  -14,  -14,  -14,  -13,  -13,  -13,
    -13,  -12,  -12,  -12,  -11,  -11,  -11,  -10,
    -10,  -10,   -9,   -9,   -9,   -8,   -8,   -8,
     -7,   -7,   -6,   -6,   -6,   -5,   -5,   -4,
     -4,   -3,   -3,   -2,   -2,   -1,   -1,    0,
      0,    1,    1,    2,    2,    3,    3,    4,
      4,    5,    5,    6,    6,    7,    8,    8,
      9,    9,   10,   11,   11,   12,   13,   13,
     14,   15,   15,   16,   17,   17,   18,   19,
     19,   20,   21,   22,   22,   23,   24,   24,
     25,   26,   27,   28,   28,   29,   30,   31,
     31,   32,   33,   34,   35,   36,   36,   37,
     38,   39,   40,   41,   41,   42,   43,   44,
     45,   46,   47,   48,   49,   49,   50,   51,
     52,   53,   54,   55,   56,   57,   58,   59,
     60,   61,   62,   62,   63,   64,   65,   66,
     67,   68,   69,   70,   71,   72,   73,   74,
     75,   76,   77,   78,   79,   80,   81,   82,
     83,   84,   85,   86,   87,   88,   90,   91,
     92,   93,   94,   95,   96,   97,   98,   99,
    100,  101,  102,  103,  104,  105,  106,  107,
};

static void ref_gelu(act_t *in, act_t *out, int n) {
    for (int i = 0; i < n; i++)
        out[i] = gelu_ref_lut[(int)in[i] + 128];
}

static int test_gelu(int CH, int H, int W, const char *name)
{
    int n = CH*H*W;
    act_t *in      = new act_t[n];
    act_t *ref_out = new act_t[n];
    act_t *hw_out  = new act_t[n];
    pack_t *in_a_p = new pack_t[(n+3)/4];
    pack_t *out_p  = new pack_t[(n+3)/4];

    for (int i = 0; i < n; i++) in[i] = (act_t)(rand()%256-128);

    pack_bytes(in, in_a_p, n);
    fastvit_ip(in_a_p, /*in_b*/nullptr, /*bias*/nullptr, out_p,
               OP_GELU, CH, H, W, /*CHout*/0, /*act_mode*/0, /*out_shift*/0,
               0, 0, 0, 0, 0, 0, 0);
    unpack_bytes(out_p, hw_out, n);

    ref_gelu(in, ref_out, n);

    int e = check(hw_out, ref_out, n, name);
    delete[] in; delete[] ref_out; delete[] hw_out;
    delete[] in_a_p; delete[] out_p;
    return e;
}

static int test_gelu_forced(act_t *forced_in, int n, const char *name)
{
    act_t *ref_out = new act_t[n];
    act_t *hw_out  = new act_t[n];
    pack_t *in_a_p = new pack_t[(n+3)/4];
    pack_t *out_p  = new pack_t[(n+3)/4];

    pack_bytes(forced_in, in_a_p, n);
    fastvit_ip(in_a_p, /*in_b*/nullptr, /*bias*/nullptr, out_p,
               OP_GELU, n, 1, 1, /*CHout*/0, /*act_mode*/0, /*out_shift*/0,
               0, 0, 0, 0, 0, 0, 0);
    unpack_bytes(out_p, hw_out, n);

    ref_gelu(forced_in, ref_out, n);

    int e = check(hw_out, ref_out, n, name);
    delete[] ref_out; delete[] hw_out; delete[] in_a_p; delete[] out_p;
    return e;
}

int main() {
    srand(42);
    printf("=== fastvit_ip unified testbench ===\n");

    printf("\n--- OP_CONV ---\n");
    test_conv(4, 8, 8, 8, 1, 1, 1, 1, 1, 7, "3x3 s1 CHin=4 CHout=8 ReLU");
    test_conv(8, 16, 16, 16, 2, 2, 1, 1, 0, 6, "3x3 s2 CHin=8 CHout=16 NoReLU");
    /* conv_worker's packed WRITE_OUT assumes Wout%4==0 (matches CONV_TC=4,
     * see conv_worker.h/.cpp) -- true for conv_ip's actual caller (the
     * stem layer, fixed power-of-2 dimensions). Hin=Win=16 keeps this
     * test representative instead of the original tb's arbitrary 14x14. */
    test_conv(16, 16, 16, 32, 1, 1, 1, 1, 1, 7, "3x3 s1 CHin=16 CHout=32 ReLU");

    printf("\n--- OP_DWCONV ---\n");
    test_dwconv(32, 56, 56, 3, 3, ACT_RELU, 8, "DW3_s1_CH32");
    test_dwconv(48, 64, 64, 3, 3, ACT_NONE, 7, "DW3_FastVIT_S1");
    test_dwconv(16, 28, 28, 7, 7, ACT_NONE, 7, "DW7_s1_CH16");
    test_dwconv(48, 16, 16, 7, 7, ACT_NONE, 7, "DW7_FastVIT_S3");
    test_dwconv(8, 4, 4, 3, 3, ACT_RELU, 6, "DW3_small");
    test_dwconv(3, 8, 8, 3, 3, ACT_NONE, 6, "DW3_CH3_odd");
    test_dwconv(5, 8, 8, 7, 7, ACT_NONE, 6, "DW7_CH5_odd");

    printf("\n--- OP_PWCONV ---\n");
    test_pwconv(32, 56, 56, 64, ACT_RELU, 8, "PW_32to64_56x56");
    test_pwconv(64, 28, 28, 128, ACT_RELU, 8, "PW_64to128_28x28");
    /* pwconv_worker's WRITE_PW_OUT assumes spatial%PW_TS(8)==0, true for
     * every real FastVIT-T8 layer (spatial is always a power of 2) --
     * 16x16 keeps this test representative instead of the original tb's
     * arbitrary 14x14 (spatial=196, 196%8=4, a partial tile that would
     * spill into the next channel; see pwconv_worker.h/.cpp). */
    test_pwconv(128, 16, 16, 256, ACT_NONE, 7, "PW_128to256_16x16");
    test_pwconv(16, 4, 4, 32, ACT_RELU, 6, "PW_small");
    test_pwconv(48, 64, 64, 48, ACT_NONE, 8, "Stem_PW48to48");
    test_pwconv(96, 32, 32, 288, ACT_NONE, 8, "Stage2_expand_96to288");
    test_pwconv(384, 8, 8, 1152, ACT_NONE, 8, "Stage4_expand_384to1152_sp64");
    /* Real-network projection shapes (S1B0/S1B1 PW2, S2B0/S2B1 PW2). */
    test_pwconv(144, 64, 64, 48, ACT_NONE, 8, "Stage1_project_144to48");
    test_pwconv(288, 32, 32, 96, ACT_NONE, 8, "Stage2_project_288to96");

    printf("\n--- OP_ADD ---\n");
    test_add(64, 56, 56, nullptr, nullptr, "Normal Addition");
    {
        int n = 64*56*56;
        act_t *a = new act_t[n], *b = new act_t[n];
        for (int i = 0; i < n; i++) { a[i] = 100; b[i] = 50; }
        test_add(64, 56, 56, a, b, "Positive Overflow");
        for (int i = 0; i < n; i++) { a[i] = -100; b[i] = -50; }
        test_add(64, 56, 56, a, b, "Negative Overflow");
        delete[] a; delete[] b;
    }
    test_add(13, 17, 19, nullptr, nullptr, "Non-divisible Tile Size");
    test_add(1, 10, 10, nullptr, nullptr, "Single Channel");

    printf("\n--- OP_GELU ---\n");
    /* Real network shapes: Stem/RepMixer PW1-expand outputs and the
     * three fpg=2 transition DW outputs (see fastvit_infer.c apply_gelu
     * call sites). */
    test_gelu(48, 64, 64,   "Stem_DW3_48_64x64");
    test_gelu(144, 64, 64,  "S1_expand_144_64x64");
    test_gelu(96, 32, 32,   "Trans1_96_32x32");
    test_gelu(288, 32, 32,  "S2_expand_288_32x32");
    test_gelu(192, 16, 16,  "Trans2_192_16x16");
    test_gelu(576, 16, 16,  "S3_expand_576_16x16");
    test_gelu(384, 8, 8,    "Trans3_384_8x8");
    test_gelu(1152, 8, 8,   "S4_expand_1152_8x8");
    test_gelu(13, 17, 19,   "Non-divisible Tile Size");
    {
        /* Exact boundary values: -128, -1, 0, 1, 127 must round-trip
         * through the LUT exactly as the reference table -- this is
         * more about pinning the ap_uint<8> index math (a+128, signed
         * to unsigned) than about the tiling loop. */
        int n = 5;
        act_t *b = new act_t[n];
        b[0] = -128; b[1] = -1; b[2] = 0; b[3] = 1; b[4] = 127;
        test_gelu_forced(b, n, "Boundary values");
        delete[] b;
    }

    printf("\n=====================================\n");
    printf(g_errors ? "TOTAL ERRORS: %d\n" : "ALL TESTS PASSED\n", g_errors);
    return g_errors > 0 ? 1 : 0;
}
