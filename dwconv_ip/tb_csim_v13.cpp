/*============================================================
 * tb_csim_v13.cpp — csim correctness check for v13 (DSP-packed MAC)
 * Packed ap_uint<32> AXI interface (matches dwconv_ip.h since v11).
 * rand()%256-128 already exercises the -128/-127/127 extremes that
 * dsp_packed_mac2's two's-complement packing math needs to get right.
 *============================================================*/
#include "dwconv_ip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void ref_dwconv(
    act_t *feat_in, wt_t *weight, acc_t *bias, act_t *feat_out,
    int CH, int Hin, int Win, int Kh, int Kw,
    int stride_h, int stride_w, int pad_h, int pad_w, int fpg,
    int act_mode, int out_shift)
{
    int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    for (int ch = 0; ch < CH; ch++) {
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

static int test_case(int CH, int H, int W, int Kh, int Kw,
                      int stride, int fpg, int act_mode, int out_shift,
                      bool extremes, const char *name)
{
    int pad = Kh / 2;
    int Hout = (H + 2*pad - Kh) / stride + 1;
    int Wout = (W + 2*pad - Kw) / stride + 1;
    int CHout = CH * fpg;
    int n_in_words  = (CH * H * W) / 4;
    int n_out_words = (CHout * Hout * Wout) / 4;

    act_t *feat_in_flat  = new act_t[CH * H * W];
    wt_t  *weight        = new wt_t [CHout * Kh * Kw];
    acc_t *bias          = new acc_t[CHout];
    act_t *ref_out       = new act_t[CHout * Hout * Wout];
    act_t *feat_out_flat = new act_t[CHout * Hout * Wout];
    ap_uint<32> *feat_in_pk  = new ap_uint<32>[n_in_words];
    ap_uint<32> *feat_out_pk = new ap_uint<32>[n_out_words];

    for (int i = 0; i < CH*H*W; i++) {
        if (extremes) {
            /* force the boundary values that stress dsp_packed_mac2 hardest */
            int r = rand() % 4;
            feat_in_flat[i] = (r==0)? (act_t)-128 : (r==1)? (act_t)127 : (act_t)(rand()%256-128);
        } else {
            feat_in_flat[i] = (act_t)(rand()%256-128);
        }
    }
    for (int i = 0; i < CHout*Kh*Kw; i++) {
        if (extremes) {
            int r = rand() % 4;
            weight[i] = (r==0)? (wt_t)-128 : (r==1)? (wt_t)127 : (wt_t)(rand()%256-128);
        } else {
            weight[i] = (wt_t)(rand()%256-128);
        }
    }
    for (int i = 0; i < CHout; i++) bias[i] = (acc_t)(rand()%1024-512);

    for (int w = 0; w < n_in_words; w++) {
        ap_uint<32> word = 0;
        for (int b = 0; b < 4; b++)
            word.range(8*b+7, 8*b) = (ap_uint<8>)feat_in_flat[w*4+b];
        feat_in_pk[w] = word;
    }

    ref_dwconv(feat_in_flat, weight, bias, ref_out,
               CH, H, W, Kh, Kw, stride, stride, pad, pad, fpg, act_mode, out_shift);
    dwconv_ip(feat_in_pk, weight, bias, feat_out_pk,
              CH, H, W, Kh, Kw, stride, stride, pad, pad, fpg, act_mode, out_shift);

    for (int w = 0; w < n_out_words; w++) {
        ap_uint<32> word = feat_out_pk[w];
        for (int b = 0; b < 4; b++)
            feat_out_flat[w*4+b] = (act_t)word.range(8*b+7, 8*b);
    }

    int errors = 0;
    for (int i = 0; i < CHout*Hout*Wout; i++) {
        if (feat_out_flat[i] != ref_out[i]) {
            errors++;
            if (errors <= 5)
                printf("  MISMATCH[%d]: got=%d ref=%d\n", i, (int)feat_out_flat[i], (int)ref_out[i]);
        }
    }
    printf("[%s] CH=%d H=%d W=%d K=%d stride=%d fpg=%d | %s\n",
           name, CH, H, W, Kh, stride, fpg, errors==0?"PASS":"FAIL");

    delete[] feat_in_flat; delete[] weight; delete[] bias;
    delete[] ref_out; delete[] feat_out_flat;
    delete[] feat_in_pk; delete[] feat_out_pk;
    return errors;
}

int main() {
    srand(42);
    int err = 0;
    err += test_case(48, 64, 64, 7, 7, 1, 1, ACT_NONE, 7, false, "DW7_S1B0_random");
    err += test_case(48, 64, 64, 7, 7, 1, 1, ACT_NONE, 7, true,  "DW7_S1B0_extremes");
    err += test_case(48, 64, 64, 3, 3, 1, 1, ACT_RELU, 8, true,  "DW3_S1B1_extremes");
    err += test_case(96, 32, 32, 7, 7, 1, 1, ACT_NONE, 7, true,  "DW7_S2B0_extremes");
    err += test_case(48, 16, 16, 7, 7, 2, 1, ACT_NONE, 7, true,  "DW7_stride2_extremes");
    err += test_case(16, 28, 28, 3, 3, 1, 2, ACT_RELU, 6, true,  "DW3_fpg2_extremes");
    err += test_case( 8,  8,  8,  3, 3, 1, 1, ACT_NONE, 6, true,  "DW3_small_extremes");
    printf("\n=== Total errors: %d ===\n", err);
    return err > 0 ? 1 : 0;
}
