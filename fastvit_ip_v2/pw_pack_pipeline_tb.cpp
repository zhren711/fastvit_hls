// pw_pack_pipeline_tb.cpp -- ZHR-92 DSP-packing Step 2 (2026-08-29). csim
// correctness for pw_flat_pipeline_packed(), the real pw_flat_pipeline-
// shaped packed FSM (not the flat pw_reduce_packed() from Step 1 -- this
// tests the actual production-shaped state machine, including the
// writeout phase's own 2-channels-per-step design). Independent golden
// reference computed with plain loops in this file, using the SAME
// W4A5-truncated real weight bytes and the same (deliberately simple:
// bias=0, shift=4 for every channel) bias/shift -- bias/shift/clip is
// unchanged scalar code untouched by packing, so it doesn't need real
// per-channel values to exercise the actual risk (pairing/wiring); the
// weight and activation DO stay real, per instruction.
#include "pw_pack_pipeline.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

static std::vector<unsigned char> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "FATAL: cannot open %s\n", path.c_str()); exit(2); }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int8_t trunc_w8(int8_t w) { return (int8_t)(w >> 4); }   // matches pw_pack_trunc_w's 4-bit truncation, done at int8 granularity for the reference
static int8_t trunc_a8(int8_t a) { return (int8_t)(a >> 3); }   // matches pw_pack_trunc_a's 5-bit truncation

static acc_t ref_clip_shift(acc_t acc, int shift) {
    acc_t v = acc >> shift;
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return v;
}

static bool run_case(const char *tag, int cin, int cout,
                      const std::string &weight_path, const std::string &act_path) {
    auto wbytes = read_file(weight_path);
    if ((int)wbytes.size() != cout * cin) {
        fprintf(stderr, "%s: weight size mismatch %d vs %d\n", tag, (int)wbytes.size(), cout * cin);
        return false;
    }
    auto abytes = read_file(act_path);
    if ((int)abytes.size() != cin) {
        fprintf(stderr, "%s: activation size mismatch %d vs %d\n", tag, (int)abytes.size(), cin);
        return false;
    }

    static wt_t w_base[MAX_CIN * 2000];  // generously sized flat weight buffer, w_off=0
    for (int i = 0; i < cout * cin; i++) w_base[i] = wt_t((int8_t)wbytes[i]);

    static act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC];
    for (int ci = 0; ci < MAX_CIN; ci++)
        for (int r = 0; r < MAC_PR; r++)
            for (int c = 0; c < MAC_PC; c++)
                pw_patch_full[ci][r][c] = act_t(0);
    for (int ci = 0; ci < cin; ci++)
        pw_patch_full[ci][0][0] = act_t((int8_t)abytes[ci]);   // real single valid spatial lane (h=w=1), matches Step 1's B/C

    static acc_t pw_bias_cache[MAX_PW_BIAS_CACHE];
    static wt_t pw_shift_cache[MAX_PW_BIAS_CACHE];
    for (int co = 0; co < cout; co++) { pw_bias_cache[co] = acc_t(0); pw_shift_cache[co] = wt_t(4); }

    static act_t out_base[2000];
    for (int i = 0; i < cout; i++) out_base[i] = act_t(0);

    LayerDescV2 d{};
    d.cin = cin; d.cout = cout;
    d.w_off = 0; d.out_off = 0; d.out_ch_stride = 1; d.w_out = 1;
    d.use_shift_table = 1; d.out_shift = 4;

    pw_flat_pipeline_packed(d, w_base, pw_patch_full, pw_bias_cache, pw_shift_cache,
                             out_base, /*rt*/0, /*colt*/0, /*r_sz*/1, /*col_sz*/1);

    // independent golden reference, plain loops, same truncated weights
    int mismatches = 0;
    for (int co = 0; co < cout; co++) {
        long sum = 0;
        for (int ci = 0; ci < cin; ci++) {
            int8_t w4 = trunc_w8((int8_t)wbytes[co * cin + ci]);
            int8_t a5 = trunc_a8((int8_t)abytes[ci]);
            sum += (long)w4 * (long)a5;
        }
        acc_t total = (acc_t)sum + pw_bias_cache[co];
        act_t expect = (act_t)ref_clip_shift(total, 4);
        act_t got = out_base[co];
        if (expect != got) {
            if (mismatches < 5)
                printf("  [%s] mismatch co=%d expect=%d got=%d\n", tag, co, (int)expect, (int)got);
            mismatches++;
        }
    }
    bool pass = (mismatches == 0);
    printf("[%s] %s  %d/%d mismatches  (cin=%d cout=%d n_cbase=%d)\n",
           tag, pass ? "PASS" : "FAIL", mismatches, cout, cin, cout, (cin + 31) / 32);
    return pass;
}

int main() {
    printf(">>> pw_flat_pipeline_packed() vs independent golden reference, same W4A5-truncated real weights\n");
    int fails = 0;
    fails += !run_case("SEfc1_layer0051_ncbase2",  48, 768,
                        "E:/codes/microzed/fastvit_hls/weights_t8_calibrated_256/layer_0051_pwconv_weight.bin",
                        "E:/codes/microzed/fastvit_hls/accuracy_test_imgs_256/entry_77.bin");
    fails += !run_case("SEfc2_layer0050_ncbase24", 768, 48,
                        "E:/codes/microzed/fastvit_hls/weights_t8_calibrated_256/layer_0050_pwconv_weight.bin",
                        "E:/codes/microzed/fastvit_hls/accuracy_test_imgs_256/entry_75.bin");
    printf(">>> %d/2 cases failed\n", fails);
    return fails != 0;
}
