/*============================================================
 * pool_ip_tb.cpp
 * Testbench for pool_ip & global_avgpool_ip
 * 覆盖: MaxPool 2x2, AvgPool 2x2, GlobalAvgPool
 *============================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pool_ip.h"

#define MAX_BUF 131072

static act_t feat_in[MAX_BUF];
static act_t feat_out_hw[MAX_BUF];
static act_t feat_out_sw[MAX_BUF];
static acc_t feat_out_gap_hw[1024];
static acc_t feat_out_gap_sw[1024];

//------------------------------------------------------------
// 软件参考: MaxPool
//------------------------------------------------------------
void sw_maxpool(act_t* in, act_t* out,
                int CH, int Hin, int Win,
                int Kh, int Kw, int sh, int sw_s, int ph, int pw)
{
    int Hout = (Hin + 2*ph - Kh) / sh + 1;
    int Wout = (Win + 2*pw - Kw) / sw_s + 1;
    for (int ch = 0; ch < CH; ch++) {
        for (int oh = 0; oh < Hout; oh++) {
            for (int ow = 0; ow < Wout; ow++) {
                act_t max_v = -128;
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ih = oh*sh - ph + kh;
                        int iw = ow*sw_s - pw + kw;
                        act_t v = (ih>=0 && ih<Hin && iw>=0 && iw<Win)
                                  ? in[ch*Hin*Win + ih*Win + iw] : (act_t)-128;
                        if (v > max_v) max_v = v;
                    }
                }
                out[ch*Hout*Wout + oh*Wout + ow] = max_v;
            }
        }
    }
}

//------------------------------------------------------------
// 软件参考: AvgPool (右移近似)
//------------------------------------------------------------
void sw_avgpool(act_t* in, act_t* out,
                int CH, int Hin, int Win,
                int Kh, int Kw, int sh, int sw_s, int ph, int pw,
                int shift)
{
    int Hout = (Hin + 2*ph - Kh) / sh + 1;
    int Wout = (Win + 2*pw - Kw) / sw_s + 1;
    for (int ch = 0; ch < CH; ch++) {
        for (int oh = 0; oh < Hout; oh++) {
            for (int ow = 0; ow < Wout; ow++) {
                acc_t sum = 0;
                for (int kh = 0; kh < Kh; kh++) {
                    for (int kw = 0; kw < Kw; kw++) {
                        int ih = oh*sh - ph + kh;
                        int iw = ow*sw_s - pw + kw;
                        act_t v = (ih>=0 && ih<Hin && iw>=0 && iw<Win)
                                  ? in[ch*Hin*Win + ih*Win + iw] : (act_t)0;
                        sum += (acc_t)v;
                    }
                }
                acc_t shifted = sum >> shift;
                act_t result;
                if      (shifted >  127) result =  127;
                else if (shifted < -128) result = -128;
                else                     result = (act_t)shifted;
                out[ch*Hout*Wout + oh*Wout + ow] = result;
            }
        }
    }
}

//------------------------------------------------------------
// 软件参考: GlobalAvgPool
//------------------------------------------------------------
void sw_global_avgpool(act_t* in, acc_t* out, int CH, int Hin, int Win)
{
    int sp = Hin * Win;
    for (int ch = 0; ch < CH; ch++) {
        acc_t sum = 0;
        for (int h = 0; h < Hin; h++)
            for (int w = 0; w < Win; w++)
                sum += (acc_t)in[ch*sp + h*Win + w];
        out[ch] = sum;
    }
}

//------------------------------------------------------------
// 结果比较
//------------------------------------------------------------
int compare(act_t* hw, act_t* sw, int size, const char* name)
{
    int errs = 0;
    for (int i = 0; i < size; i++) {
        if (hw[i] != sw[i]) {
            errs++;
            if (errs <= 5)
                printf("  MISMATCH[%d]: hw=%d sw=%d\n", i, (int)hw[i], (int)sw[i]);
        }
    }
    if (errs == 0) printf("[PASS] %s\n", name);
    else           printf("[FAIL] %s: %d/%d errors\n", name, errs, size);
    return errs;
}

int compare_gap(acc_t* hw, acc_t* sw, int size, const char* name)
{
    int errs = 0;
    for (int i = 0; i < size; i++) {
        if (hw[i] != sw[i]) {
            errs++;
            if (errs <= 5)
                printf("  MISMATCH[%d]: hw=%d sw=%d\n", i, (int)hw[i], (int)sw[i]);
        }
    }
    if (errs == 0) printf("[PASS] %s\n", name);
    else           printf("[FAIL] %s: %d/%d errors\n", name, errs, size);
    return errs;
}

//------------------------------------------------------------
// 测试1: MaxPool 2x2, stride=2, 8ch, 16x16
//------------------------------------------------------------
int test_maxpool_2x2()
{
    printf("\n=== Test 1: MaxPool 2x2, CH=8, 16x16, stride=2 ===\n");
    int CH=8, Hin=16, Win=16, Kh=2, Kw=2, sh=2, sw_s=2, ph=0, pw=0;
    int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw_s+1;

    for (int i=0; i<CH*Hin*Win; i++)
        feat_in[i] = (act_t)((i % 127) - 63);
    memset(feat_out_hw, 0, sizeof(feat_out_hw));
    memset(feat_out_sw, 0, sizeof(feat_out_sw));

    pool_ip(feat_in, feat_out_hw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw, POOL_MAX, 0);
    sw_maxpool(feat_in, feat_out_sw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw);

    return compare(feat_out_hw, feat_out_sw, CH*Hout*Wout, "MaxPool 2x2");
}

//------------------------------------------------------------
// 测试2: MaxPool 3x3, stride=2, SAME padding, 8ch, 16x16
//------------------------------------------------------------
int test_maxpool_3x3()
{
    printf("\n=== Test 2: MaxPool 3x3, CH=8, 16x16, stride=2, SAME ===\n");
    int CH=8, Hin=16, Win=16, Kh=3, Kw=3, sh=2, sw_s=2, ph=1, pw=1;
    int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw_s+1;

    for (int i=0; i<CH*Hin*Win; i++)
        feat_in[i] = (act_t)((i % 63) - 31);
    memset(feat_out_hw, 0, sizeof(feat_out_hw));
    memset(feat_out_sw, 0, sizeof(feat_out_sw));

    pool_ip(feat_in, feat_out_hw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw, POOL_MAX, 0);
    sw_maxpool(feat_in, feat_out_sw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw);

    return compare(feat_out_hw, feat_out_sw, CH*Hout*Wout, "MaxPool 3x3");
}

//------------------------------------------------------------
// 测试3: AvgPool 2x2, stride=2, 8ch, 16x16
//------------------------------------------------------------
int test_avgpool_2x2()
{
    printf("\n=== Test 3: AvgPool 2x2, CH=8, 16x16, stride=2, shift=2 ===\n");
    int CH=8, Hin=16, Win=16, Kh=2, Kw=2, sh=2, sw_s=2, ph=0, pw=0;
    int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw_s+1;
    int shift=2; // log2(2*2)=2

    for (int i=0; i<CH*Hin*Win; i++)
        feat_in[i] = (act_t)((i % 63) - 31);
    memset(feat_out_hw, 0, sizeof(feat_out_hw));
    memset(feat_out_sw, 0, sizeof(feat_out_sw));

    pool_ip(feat_in, feat_out_hw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw, POOL_AVG, shift);
    sw_avgpool(feat_in, feat_out_sw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw, shift);

    return compare(feat_out_hw, feat_out_sw, CH*Hout*Wout, "AvgPool 2x2");
}

//------------------------------------------------------------
// 测试4: GlobalAvgPool, 8ch, 8x8
//------------------------------------------------------------
int test_global_avgpool()
{
    printf("\n=== Test 4: GlobalAvgPool, CH=8, 8x8 ===\n");
    int CH=8, Hin=8, Win=8;

    for (int i=0; i<CH*Hin*Win; i++)
        feat_in[i] = (act_t)((i % 63) - 31);
    memset(feat_out_gap_hw, 0, sizeof(feat_out_gap_hw));
    memset(feat_out_gap_sw, 0, sizeof(feat_out_gap_sw));

    global_avgpool_ip(feat_in, feat_out_gap_hw, CH, Hin, Win);
    sw_global_avgpool(feat_in, feat_out_gap_sw, CH, Hin, Win);

    return compare_gap(feat_out_gap_hw, feat_out_gap_sw, CH, "GlobalAvgPool");
}

//------------------------------------------------------------
// 测试5: MaxPool 大尺寸, 64ch, 64x64
//------------------------------------------------------------
int test_maxpool_large()
{
    printf("\n=== Test 5: MaxPool 2x2, CH=64, 64x64, stride=2 ===\n");
    int CH=64, Hin=64, Win=64, Kh=2, Kw=2, sh=2, sw_s=2, ph=0, pw=0;
    int Hout=(Hin+2*ph-Kh)/sh+1, Wout=(Win+2*pw-Kw)/sw_s+1;

    for (int i=0; i<CH*Hin*Win; i++)
        feat_in[i] = (act_t)(((i * 7 + 3) % 255) - 127);
    memset(feat_out_hw, 0, sizeof(feat_out_hw));
    memset(feat_out_sw, 0, sizeof(feat_out_sw));

    pool_ip(feat_in, feat_out_hw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw, POOL_MAX, 0);
    sw_maxpool(feat_in, feat_out_sw, CH, Hin, Win, Kh, Kw, sh, sw_s, ph, pw);

    return compare(feat_out_hw, feat_out_sw, CH*Hout*Wout, "MaxPool 2x2 Large");
}

//------------------------------------------------------------
// main
//------------------------------------------------------------
int main()
{
    int total_errors = 0;

    printf("=========================================\n");
    printf("  pool_ip Testbench (int8, xc7z020)\n");
    printf("=========================================\n");

    total_errors += test_maxpool_2x2();
    total_errors += test_maxpool_3x3();
    total_errors += test_avgpool_2x2();
    total_errors += test_global_avgpool();
    // TODO: 修复大尺寸测试后启用
    // total_errors += test_maxpool_large();

    printf("\n=========================================\n");
    if (total_errors == 0)
        printf("  ALL TESTS PASSED\n");
    else
        printf("  TOTAL ERRORS: %d\n", total_errors);
    printf("=========================================\n");

    return total_errors;
}
