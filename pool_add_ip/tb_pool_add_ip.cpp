/*============================================================
 * tb_pool_add_ip.cpp
 * Testbench for pool_add_ip
 * 测试用例:
 *   TC1: MaxPool 3x3 s2p1, CH=4, 8x8 → 4x4, add_en=0
 *   TC2: AvgPool 3x3 s2p1, CH=4, 8x8 → 4x4, add_en=1 (加残差)
 *   TC3: GlobalAvgPool, CH=4, 4x4, add_en=0
 *============================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pool_add_ip.h"

#define MAX_SIZE 65536

static act_t feat_in [MAX_SIZE];
static act_t feat_res[MAX_SIZE];
static act_t feat_out[MAX_SIZE];
static act_t ref_out [MAX_SIZE];

//------------------------------------------------------------
// 参考实现: MaxPool
//------------------------------------------------------------
static void ref_maxpool(
    act_t* in, act_t* res, act_t* out,
    int CH, int Hin, int Win,
    int Kh, int Kw, int sh, int sw, int ph, int pw,
    int add_en)
{
    int Hout = (Hin + 2*ph - Kh) / sh + 1;
    int Wout = (Win + 2*pw - Kw) / sw + 1;
    for (int ch = 0; ch < CH; ch++) {
        for (int r = 0; r < Hout; r++) {
            for (int c = 0; c < Wout; c++) {
                int8_t mx = -128;
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ir = r*sh - ph + kh;
                        int ic = c*sw - pw + kw;
                        int8_t v = (ir>=0&&ir<Hin&&ic>=0&&ic<Win)
                                    ? (int8_t)in[ch*Hin*Win+ir*Win+ic]
                                    : -128;
                        if (v > mx) mx = v;
                    }
                }
                int idx = ch*Hout*Wout + r*Wout + c;
                int sum = (int)mx + (add_en ? (int)(int8_t)res[idx] : 0);
                if      (sum >  127) sum =  127;
                else if (sum < -128) sum = -128;
                out[idx] = (act_t)sum;
            }
        }
    }
}

//------------------------------------------------------------
// 参考实现: AvgPool
//------------------------------------------------------------
static void ref_avgpool(
    act_t* in, act_t* res, act_t* out,
    int CH, int Hin, int Win,
    int Kh, int Kw, int sh, int sw, int ph, int pw,
    int add_en, int shift)
{
    int Hout = (Hin + 2*ph - Kh) / sh + 1;
    int Wout = (Win + 2*pw - Kw) / sw + 1;
    for (int ch = 0; ch < CH; ch++) {
        for (int r = 0; r < Hout; r++) {
            for (int c = 0; c < Wout; c++) {
                int acc = 0;
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ir = r*sh - ph + kh;
                        int ic = c*sw - pw + kw;
                        if (ir>=0&&ir<Hin&&ic>=0&&ic<Win)
                            acc += (int)(int8_t)in[ch*Hin*Win+ir*Win+ic];
                    }
                }
                int pool_v = acc >> shift;
                if      (pool_v >  127) pool_v =  127;
                else if (pool_v < -128) pool_v = -128;
                int idx = ch*Hout*Wout + r*Wout + c;
                int sum = pool_v + (add_en ? (int)(int8_t)res[idx] : 0);
                if      (sum >  127) sum =  127;
                else if (sum < -128) sum = -128;
                out[idx] = (act_t)sum;
            }
        }
    }
}

//------------------------------------------------------------
// 参考实现: GlobalAvgPool
//------------------------------------------------------------
static void ref_gap(
    act_t* in, act_t* res, act_t* out,
    int CH, int Hin, int Win, int add_en, int shift)
{
    for (int ch = 0; ch < CH; ch++) {
        int acc = 0;
        for (int h = 0; h < Hin; h++)
            for (int w = 0; w < Win; w++)
                acc += (int)(int8_t)in[ch*Hin*Win+h*Win+w];
        int pool_v = acc >> shift;
        if      (pool_v >  127) pool_v =  127;
        else if (pool_v < -128) pool_v = -128;
        int sum = pool_v + (add_en ? (int)(int8_t)res[ch] : 0);
        if      (sum >  127) sum =  127;
        else if (sum < -128) sum = -128;
        out[ch] = (act_t)sum;
    }
}

//------------------------------------------------------------
// 比较工具
//------------------------------------------------------------
static int check(act_t* out, act_t* ref, int n, const char* name) {
    int errs = 0;
    for (int i = 0; i < n; i++) {
        if ((int8_t)out[i] != (int8_t)ref[i]) {
            if (errs < 5)
                printf("  [%s] MISMATCH idx=%d out=%d ref=%d\n",
                       name, i, (int)(int8_t)out[i], (int)(int8_t)ref[i]);
            errs++;
        }
    }
    if (errs == 0) printf("  [%s] PASS\n", name);
    else           printf("  [%s] FAIL (%d errors)\n", name, errs);
    return errs;
}

//------------------------------------------------------------
// main
//------------------------------------------------------------
int main() {
    int total_err = 0;
    srand(42);

    // ---- TC1: MaxPool 3x3 s2p1, CH=4, 8x8 → 4x4, add_en=0 ----
    {
        int CH=4, Hin=8, Win=8, Kh=3, Kw=3, sh=2, sw=2, ph=1, pw=1;
        int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw+1;
        int in_n=CH*Hin*Win, out_n=CH*Hout*Wout;
        for (int i=0;i<in_n;i++) feat_in[i]=(act_t)(rand()%256-128);
        memset(feat_out, 0, out_n);
        ref_maxpool(feat_in, feat_res, ref_out, CH,Hin,Win,Kh,Kw,sh,sw,ph,pw, 0);
        pool_add_ip(feat_in, feat_res, feat_out, CH,Hin,Win,Kh,Kw,sh,sw,ph,pw,
                    POOL_MAX, ADD_DISABLE, 0);
        total_err += check(feat_out, ref_out, out_n, "TC1 MaxPool no-add");
    }

    // ---- TC2: AvgPool 3x3 s2p1, CH=4, 8x8 → 4x4, add_en=1 ----
    {
        int CH=4, Hin=8, Win=8, Kh=3, Kw=3, sh=2, sw=2, ph=1, pw=1, shift=3;
        int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw+1;
        int in_n=CH*Hin*Win, out_n=CH*Hout*Wout;
        for (int i=0;i<in_n;i++) feat_in[i]=(act_t)(rand()%256-128);
        for (int i=0;i<out_n;i++) feat_res[i]=(act_t)(rand()%256-128);
        memset(feat_out, 0, out_n);
        ref_avgpool(feat_in, feat_res, ref_out, CH,Hin,Win,Kh,Kw,sh,sw,ph,pw, 1, shift);
        pool_add_ip(feat_in, feat_res, feat_out, CH,Hin,Win,Kh,Kw,sh,sw,ph,pw,
                    POOL_AVG, ADD_ENABLE, shift);
        total_err += check(feat_out, ref_out, out_n, "TC2 AvgPool with-add");
    }

    // ---- TC3: GlobalAvgPool, CH=8, 4x4, add_en=0 ----
    {
        int CH=8, Hin=4, Win=4, shift=4;
        int in_n=CH*Hin*Win;
        for (int i=0;i<in_n;i++) feat_in[i]=(act_t)(rand()%256-128);
        memset(feat_out, 0, CH);
        ref_gap(feat_in, feat_res, ref_out, CH,Hin,Win, 0, shift);
        pool_add_ip(feat_in, feat_res, feat_out, CH,Hin,Win,
                    Hin,Win,1,1,0,0,
                    POOL_GLOBAL, ADD_DISABLE, shift);
        total_err += check(feat_out, ref_out, CH, "TC3 GlobalAvgPool no-add");
    }

    // ---- TC4: MaxPool 2x2 s2p0, CH=4, 8x8 → 4x4, add_en=1 ----
    {
        int CH=4, Hin=8, Win=8, Kh=2, Kw=2, sh=2, sw=2, ph=0, pw=0;
        int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw+1;
        int in_n=CH*Hin*Win, out_n=CH*Hout*Wout;
        for (int i=0;i<in_n;i++) feat_in[i]=(act_t)(rand()%256-128);
        for (int i=0;i<out_n;i++) feat_res[i]=(act_t)(rand()%256-128);
        memset(feat_out, 0, out_n);
        ref_maxpool(feat_in, feat_res, ref_out, CH,Hin,Win,Kh,Kw,sh,sw,ph,pw, 1);
        pool_add_ip(feat_in, feat_res, feat_out, CH,Hin,Win,Kh,Kw,sh,sw,ph,pw,
                    POOL_MAX, ADD_ENABLE, 0);
        total_err += check(feat_out, ref_out, out_n, "TC4 MaxPool 2x2 with-add");
    }

    printf("\n=== Total errors: %d ===\n", total_err);
    return total_err;
}
