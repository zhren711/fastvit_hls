/*============================================================
 * tb_pwconv_ip.cpp
 * Pointwise (1×1) Convolution IP 测试台
 *============================================================*/

#include "pwconv_ip.h"
#include <cstdio>
#include <cstdlib>

static void ref_pwconv(
    act_t *feat_in, wt_t *weight, acc_t *bias, act_t *feat_out,
    int CHin, int H, int W, int CHout, int act_mode, int out_shift)
{
    int spatial = H * W;
    for (int co = 0; co < CHout; co++) {
        for (int s = 0; s < spatial; s++) {
            acc_t sum = bias[co];
            for (int ci = 0; ci < CHin; ci++) {
                sum += (acc_t)feat_in[ci * spatial + s] *
                       (acc_t)weight[co * CHin + ci];
            }
            acc_t shifted = sum >> out_shift;
            act_t v;
            if      (shifted >  127) v =  127;
            else if (shifted < -128) v = -128;
            else                     v = (act_t)shifted;
            if (act_mode == ACT_RELU && v < 0) v = 0;
            feat_out[co * spatial + s] = v;
        }
    }
}

static int test_case(
    int CHin, int H, int W, int CHout,
    int act_mode, int out_shift, const char *name)
{
    int spatial = H * W;
    act_t *feat_in  = new act_t[CHin  * spatial];
    wt_t  *weight   = new wt_t [CHout * CHin];
    acc_t *bias     = new acc_t[CHout];
    act_t *feat_out = new act_t[CHout * spatial];
    act_t *ref_out  = new act_t[CHout * spatial];

    for (int i = 0; i < CHin  * spatial; i++) feat_in[i] = (act_t)(rand() % 256 - 128);
    for (int i = 0; i < CHout * CHin;    i++) weight[i]  = (wt_t) (rand() % 256 - 128);
    for (int i = 0; i < CHout;           i++) bias[i]    = (acc_t)(rand() % 1024 - 512);

    ref_pwconv(feat_in, weight, bias, ref_out, CHin, H, W, CHout, act_mode, out_shift);
    pwconv_ip (feat_in, weight, bias, feat_out, CHin, H, W, CHout, act_mode, out_shift);

    int errors = 0;
    for (int i = 0; i < CHout * spatial; i++) {
        if (feat_out[i] != ref_out[i]) {
            errors++;
            if (errors <= 5)
                printf("  MISMATCH[%d]: got=%d, ref=%d\n", i, (int)feat_out[i], (int)ref_out[i]);
        }
    }
    printf("[%s] CHin=%d CHout=%d H=%d W=%d → %s\n",
           name, CHin, CHout, H, W, errors == 0 ? "PASS" : "FAIL");

    delete[] feat_in; delete[] weight; delete[] bias;
    delete[] feat_out; delete[] ref_out;
    return errors;
}

int main() {
    srand(42);
    int total = 0;

    // 基础功能测试
    total += test_case(32,  56, 56,  64, ACT_RELU, 8, "PW_32to64_56x56");
    total += test_case(64,  28, 28, 128, ACT_RELU, 8, "PW_64to128_28x28");
    total += test_case(128, 14, 14, 256, ACT_NONE, 7, "PW_128to256_14x14");
    total += test_case(16,   4,  4,  32, ACT_RELU, 6, "PW_small");

    // FastVIT-SA36 实际层（验证 BRAM 寻址和循环边界）
    total += test_case(48,  64, 64,  48, ACT_NONE, 8, "Stem_PW48to48");
    total += test_case(48,  64, 64, 144, ACT_NONE, 8, "Stage1_expand_48to144");
    total += test_case(144, 64, 64,  48, ACT_NONE, 8, "Stage1_compress_144to48");
    total += test_case(96,  32, 32, 288, ACT_NONE, 8, "Stage2_expand_96to288");
    total += test_case(192, 16, 16, 576, ACT_NONE, 8, "Stage3_expand_192to576");
    total += test_case(576, 16, 16, 192, ACT_NONE, 8, "Stage3_compress_576to192");

    // Stage4: spatial=64 < PW_TS=128 (关键边界: BRAM 尾部填充)
    total += test_case(384,  8,  8, 1152, ACT_NONE, 8, "Stage4_expand_384to1152_sp64");
    total += test_case(1152, 8,  8,  384, ACT_NONE, 8, "Stage4_compress_1152to384_sp64");

    printf("\n=== Total errors: %d ===\n", total);
    return total > 0 ? 1 : 0;
}
