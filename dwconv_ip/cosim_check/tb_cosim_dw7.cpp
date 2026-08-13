/*============================================================
 * tb_cosim_dw7.cpp — minimal single-case testbench for RTL cosim
 * Mirrors S1B0 DW7 structurally (CH=48, K=7, fpg=1, SAME pad)
 * but with H=W=16 (instead of 64) to keep cosim runtime bounded.
 * Goal: measure actual RTL cycles/tile and cycles/channel-boundary
 * to compare against the ~84-95 cycles/tile HLS static estimate.
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

#define CH 48
#define H  16
#define W  16
#define KH 7
#define KW 7
#define N_WORDS ((CH*H*W)/4)

static act_t       feat_in_flat [CH*H*W];
static wt_t        weight       [CH*KH*KW];
static acc_t       bias         [CH];
static act_t       ref_out      [CH*H*W];
static act_t       feat_out_flat[CH*H*W];
static ap_uint<32> feat_in_pk   [N_WORDS];
static ap_uint<32> feat_out_pk  [N_WORDS];

int main() {
    srand(42);
    const int Kh = KH, Kw = KW;
    const int pad = Kh / 2;
    const int n_words = N_WORDS;

    for (int i = 0; i < CH*H*W;   i++) feat_in_flat[i] = (act_t)(rand()%256-128);
    for (int i = 0; i < CH*Kh*Kw; i++) weight[i]       = (wt_t) (rand()%256-128);
    for (int i = 0; i < CH;       i++) bias[i]         = (acc_t)(rand()%1024-512);

    /* pack: matches PRELOAD_IN's unpack order (byte0=LSB=first pixel) */
    for (int w = 0; w < n_words; w++) {
        ap_uint<32> word = 0;
        for (int b = 0; b < 4; b++)
            word.range(8*b+7, 8*b) = (ap_uint<8>)feat_in_flat[w*4+b];
        feat_in_pk[w] = word;
    }

    ref_dwconv(feat_in_flat, weight, bias, ref_out, CH, H, W, Kh, Kw, pad, ACT_NONE, 7);
    dwconv_ip(feat_in_pk, weight, bias, feat_out_pk,
              CH, H, W, Kh, Kw, 1, 1, pad, pad, /*fpg=*/1, ACT_NONE, 7);

    for (int w = 0; w < n_words; w++) {
        ap_uint<32> word = feat_out_pk[w];
        for (int b = 0; b < 4; b++)
            feat_out_flat[w*4+b] = (act_t)word.range(8*b+7, 8*b);
    }

    int errors = 0;
    for (int i = 0; i < CH*H*W; i++) {
        if (feat_out_flat[i] != ref_out[i]) {
            errors++;
            if (errors <= 5)
                printf("  MISMATCH[%d]: got=%d ref=%d\n", i, (int)feat_out_flat[i], (int)ref_out[i]);
        }
    }
    printf("[COSIM_DW7_CH48_16x16] %s\n", errors==0 ? "PASS" : "FAIL");

    return errors > 0 ? 1 : 0;
}
