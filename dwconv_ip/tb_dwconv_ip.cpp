/*============================================================
 * tb_dwconv_ip.cpp  v2.0
 * stride=1 hardcoded, pad=Kh/2 (SAME)
 *============================================================*/

#include "dwconv_ip.h"
#include <cstdio>
#include <cstdlib>

static void ref_dwconv(
    act_t *feat_in, wt_t *weight, acc_t *bias, act_t *feat_out,
    int CH, int Hin, int Win, int Kh, int Kw, int pad,
    int act_mode, int out_shift)
{
    for (int ch = 0; ch < CH; ch++) {
        for (int oh = 0; oh < Hin; oh++) {
            for (int ow = 0; ow < Win; ow++) {
                acc_t sum = bias[ch];
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ih = oh - pad + kh;
                        int iw = ow - pad + kw;
                        if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                            sum += (acc_t)feat_in[ch*Hin*Win + ih*Win + iw] *
                                   (acc_t)weight[ch*Kh*Kw + kh*Kw + kw];
                    }
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
    }
}

static int test_case(int CH, int H, int W, int Kh, int Kw,
                     int act_mode, int out_shift, const char *name)
{
    int pad = Kh / 2;
    act_t *feat_in  = new act_t[CH * H * W];
    wt_t  *weight   = new wt_t [CH * Kh * Kw];
    acc_t *bias     = new acc_t[CH];
    act_t *feat_out = new act_t[CH * H * W];
    act_t *ref_out  = new act_t[CH * H * W];

    for (int i = 0; i < CH*H*W;    i++) feat_in[i] = (act_t)(rand()%256-128);
    for (int i = 0; i < CH*Kh*Kw;  i++) weight[i]  = (wt_t) (rand()%256-128);
    for (int i = 0; i < CH;         i++) bias[i]    = (acc_t)(rand()%1024-512);

    ref_dwconv(feat_in, weight, bias, ref_out,
               CH, H, W, Kh, Kw, pad, act_mode, out_shift);
    dwconv_ip(feat_in, weight, bias, feat_out,
              CH, H, W, Kh, Kw, 1, 1, pad, pad,
              act_mode, out_shift);

    int errors = 0;
    for (int i = 0; i < CH*H*W; i++) {
        if (feat_out[i] != ref_out[i]) {
            errors++;
            if (errors <= 5)
                printf("  MISMATCH[%d]: got=%d ref=%d\n", i, (int)feat_out[i], (int)ref_out[i]);
        }
    }
    printf("[%s] CH=%d H=%d W=%d K=%d pad=%d | %s\n",
           name, CH, H, W, Kh, pad, errors==0?"PASS":"FAIL");

    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] feat_out; delete[] ref_out;
    return errors;
}

int main() {
    srand(42);
    int err = 0;
    err += test_case(32, 56, 56, 3, 3, ACT_RELU, 8, "DW3_s1_CH32");
    err += test_case(48, 64, 64, 3, 3, ACT_NONE, 7, "DW3_FastVIT_S1");
    err += test_case(16, 28, 28, 7, 7, ACT_NONE, 7, "DW7_s1_CH16");
    err += test_case(48, 16, 16, 7, 7, ACT_NONE, 7, "DW7_FastVIT_S3");
    err += test_case( 8,  4,  4, 3, 3, ACT_RELU, 6, "DW3_small");
    /* TN=2 奇数 channel 边界测试 */
    err += test_case( 3, 8,  8,  3, 3, ACT_NONE, 6, "DW3_CH3_odd");
    err += test_case( 5, 8,  8,  7, 7, ACT_NONE, 6, "DW7_CH5_odd");
    printf("\n=== Total errors: %d ===\n", err);
    return err > 0 ? 1 : 0;
}
