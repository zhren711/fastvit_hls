/*============================================================
 * conv_ip_tb.cpp  v2 — Standard 3×3 Conv testbench
 *============================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conv_ip.h"

#define MAX_BUF 262144

static act_t feat_in [MAX_BUF];
static act_t feat_out_hw[MAX_BUF];
static act_t feat_out_sw[MAX_BUF];
static wt_t  weight_buf[MAX_BUF];
static acc_t bias_buf[MAX_CH];

//------------------------------------------------------------
// SW reference: Standard 3×3 Conv
//------------------------------------------------------------
void sw_conv3x3(act_t* in, wt_t* wt, acc_t* bias, act_t* out,
                int CHin, int Hin, int Win, int CHout,
                int sh, int sw_s, int ph, int pw,
                int relu, int shift)
{
    int Hout = (Hin + 2*ph - CONV_K) / sh + 1;
    int Wout = (Win + 2*pw - CONV_K) / sw_s + 1;
    for (int co = 0; co < CHout; co++) {
        for (int oh = 0; oh < Hout; oh++) {
            for (int ow = 0; ow < Wout; ow++) {
                acc_t acc = bias[co];
                for (int ci = 0; ci < CHin; ci++) {
                    for (int kh = 0; kh < CONV_K; kh++) {
                        for (int kw = 0; kw < CONV_K; kw++) {
                            int ih = oh*sh - ph + kh;
                            int iw = ow*sw_s - pw + kw;
                            act_t f = (ih>=0&&ih<Hin&&iw>=0&&iw<Win)
                                       ? in[ci*Hin*Win + ih*Win + iw] : (act_t)0;
                            wt_t  w = wt[co*CHin*CONV_K*CONV_K + ci*CONV_K*CONV_K + kh*CONV_K + kw];
                            acc += (acc_t)f * (acc_t)w;
                        }
                    }
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
    }
}

int check(act_t* hw, act_t* sw, int n, const char* name) {
    int err = 0;
    for (int i = 0; i < n; i++) {
        if (hw[i] != sw[i]) {
            err++;
            if (err <= 5) printf("  MISMATCH[%d]: hw=%d sw=%d\n", i, (int)hw[i], (int)sw[i]);
        }
    }
    printf("[%s] %s (%d points)\n", err ? "FAIL" : "PASS", name, n);
    return err;
}

//------------------------------------------------------------
// Test 1: 3x3, CHin=4, CHout=8, 8x8, stride=1, SAME, ReLU
//------------------------------------------------------------
int test1() {
    int CHin=4, Hin=8, Win=8, CHout=8, sh=1, sw_s=1, ph=1, pw=1, shift=7;
    int Hout=(Hin+2*ph-CONV_K)/sh+1, Wout=(Win+2*pw-CONV_K)/sw_s+1;
    for (int i=0;i<CHin*Hin*Win;i++) feat_in[i]=(act_t)((i%127)-63);
    for (int i=0;i<CHout*CHin*CONV_K*CONV_K;i++) weight_buf[i]=(wt_t)((i%7)-3);
    for (int i=0;i<CHout;i++) bias_buf[i]=(acc_t)(i*16);
    memset(feat_out_hw,0,sizeof(feat_out_hw));
    memset(feat_out_sw,0,sizeof(feat_out_sw));
    conv_ip(feat_in,weight_buf,bias_buf,feat_out_hw,CHin,Hin,Win,CHout,sh,sw_s,ph,pw,1,shift);
    sw_conv3x3(feat_in,weight_buf,bias_buf,feat_out_sw,CHin,Hin,Win,CHout,sh,sw_s,ph,pw,1,shift);
    return check(feat_out_hw,feat_out_sw,CHout*Hout*Wout,"3x3 s1 CHin=4 CHout=8 ReLU");
}

//------------------------------------------------------------
// Test 2: 3x3, CHin=8, CHout=16, 16x16, stride=2, SAME
//------------------------------------------------------------
int test2() {
    int CHin=8, Hin=16, Win=16, CHout=16, sh=2, sw_s=2, ph=1, pw=1, shift=6;
    int Hout=(Hin+2*ph-CONV_K)/sh+1, Wout=(Win+2*pw-CONV_K)/sw_s+1;
    for (int i=0;i<CHin*Hin*Win;i++) feat_in[i]=(act_t)((i%63)-31);
    for (int i=0;i<CHout*CHin*CONV_K*CONV_K;i++) weight_buf[i]=(wt_t)((i%5)-2);
    for (int i=0;i<CHout;i++) bias_buf[i]=(acc_t)0;
    memset(feat_out_hw,0,sizeof(feat_out_hw));
    memset(feat_out_sw,0,sizeof(feat_out_sw));
    conv_ip(feat_in,weight_buf,bias_buf,feat_out_hw,CHin,Hin,Win,CHout,sh,sw_s,ph,pw,0,shift);
    sw_conv3x3(feat_in,weight_buf,bias_buf,feat_out_sw,CHin,Hin,Win,CHout,sh,sw_s,ph,pw,0,shift);
    return check(feat_out_hw,feat_out_sw,CHout*Hout*Wout,"3x3 s2 CHin=8 CHout=16 NoReLU");
}

//------------------------------------------------------------
// Test 3: 较大输入，CHin=16, CHout=32, 14x14, stride=1, SAME
//------------------------------------------------------------
int test3() {
    int CHin=16, Hin=14, Win=14, CHout=32, sh=1, sw_s=1, ph=1, pw=1, shift=7;
    int Hout=(Hin+2*ph-CONV_K)/sh+1, Wout=(Win+2*pw-CONV_K)/sw_s+1;
    for (int i=0;i<CHin*Hin*Win;i++) feat_in[i]=(act_t)((i%31)-15);
    for (int i=0;i<CHout*CHin*CONV_K*CONV_K;i++) weight_buf[i]=(wt_t)((i%3)-1);
    for (int i=0;i<CHout;i++) bias_buf[i]=(acc_t)(i%64);
    memset(feat_out_hw,0,sizeof(feat_out_hw));
    memset(feat_out_sw,0,sizeof(feat_out_sw));
    conv_ip(feat_in,weight_buf,bias_buf,feat_out_hw,CHin,Hin,Win,CHout,sh,sw_s,ph,pw,1,shift);
    sw_conv3x3(feat_in,weight_buf,bias_buf,feat_out_sw,CHin,Hin,Win,CHout,sh,sw_s,ph,pw,1,shift);
    return check(feat_out_hw,feat_out_sw,CHout*Hout*Wout,"3x3 s1 CHin=16 CHout=32 ReLU");
}

int main() {
    printf("=== conv_ip v2 testbench (3x3 Standard Conv only) ===\n");
    int errs = 0;
    errs += test1();
    errs += test2();
    errs += test3();
    printf("=====================================================\n");
    printf(errs ? "TOTAL ERRORS: %d\n" : "ALL TESTS PASSED\n", errs);
    return errs;
}
