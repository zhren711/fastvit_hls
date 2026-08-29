// pw_pack_reduce_tb.cpp -- ZHR-92 DSP-packing Step 1 (2026-08-29). csim
// correctness: pw_reduce_packed() vs pw_reduce_unpacked(), same
// W4A5-truncated real weight bytes fed to both (never real weight vs
// truncated weight -- that would test quantization error, not packing
// correctness, per instruction). 4 cases targeting the real risk (array-
// wiring/pairing at cin-chunk and cout-pair boundaries), not accuracy:
//
//   A. synthetic cin=20 (n_cbase=1, the one shape no real PW layer has --
//      real minimum cin is 48 -- included specifically to cover the
//      n_cbase=1 boundary the plan asked for), cout=8, full spatial tile
//   B. real layer_0051_pwconv (SE fc1, cin=48->cout=768): n_cbase=2 with
//      a PARTIAL last chunk (16 of 32) -- real weight bytes, real
//      activation (entry_77.bin), h=w=1 so only spatial lane (0,0) is
//      valid -- every packed pair has exactly one gated-invalid member,
//      the real hardware shape for every SE fc1/fc2 dispatch
//   C. real layer_0050_pwconv (SE fc2, cin=768->cout=48): n_cbase=24,
//      EXACT chunk boundary (768=24*32, no partial), cout=48 is the
//      minimum real cout value -- real weight bytes, real activation
//      (entry_75.bin), same h=w=1 single-valid-lane shape as B
//   D. real layer_0006_pwconv weight bytes (cin=144->cout=48): n_cbase=5
//      with a partial last chunk (16 of 32), and a FULL spatial tile
//      (last_r=last_c=4, all 16 lanes valid -- unlike B/C) -- real
//      weight bytes; activation is deterministic synthetic (this layer's
//      real input isn't captured in this project's existing dumps, and
//      the risk under test here is cin/cout/tile-pairing wiring, which
//      doesn't depend on activation VALUES, only on there being a full
//      valid 16-lane tile to pair across)
#include "pw_pack_reduce.h"
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

static int lcg_state = 0x12345;
static int8_t lcg_i8() {
    lcg_state = lcg_state * 1103515245 + 12345;
    return (int8_t)((lcg_state >> 16) & 0xFF);
}

static bool run_case(const char *tag, int cin, int cout, int last_r, int last_c,
                      const wt4_t *weight, const act5_t *patch) {
    std::vector<acc_t> acc_u(cout * MAC_PR * MAC_PC);
    std::vector<acc_t> acc_p(cout * MAC_PR * MAC_PC);

    pw_reduce_unpacked(cin, cout, last_r, last_c, weight, patch, acc_u.data());
    pw_reduce_packed(cin, cout, last_r, last_c, weight, patch, acc_p.data());

    int mismatches = 0;
    int total = cout * MAC_PR * MAC_PC;
    for (int i = 0; i < total; i++) {
        if (acc_u[i] != acc_p[i]) {
            if (mismatches < 5) {
                int co = i / (MAC_PR * MAC_PC);
                int rem = i % (MAC_PR * MAC_PC);
                printf("  [%s] mismatch co=%d pos=%d unpacked=%d packed=%d\n",
                       tag, co, rem, (int)acc_u[i], (int)acc_p[i]);
            }
            mismatches++;
        }
    }
    bool pass = (mismatches == 0);
    printf("[%s] %s  %d/%d mismatches  (cin=%d cout=%d n_cbase=%d last_r=%d last_c=%d)\n",
           tag, pass ? "PASS" : "FAIL", mismatches, total, cin, cout, (cin + 31) / 32, last_r, last_c);
    return pass;
}

int main() {
    printf(">>> pw_reduce_packed vs pw_reduce_unpacked, same W4A5-truncated weights, 4 real-shaped cases\n");
    int fails = 0;

    // A: synthetic n_cbase=1
    {
        int cin = 20, cout = 8, lr = MAC_PR, lc = MAC_PC;
        std::vector<wt4_t> w(cout * cin);
        std::vector<act5_t> patch(MAC_PR * MAC_PC * cin);
        for (int i = 0; i < cout * cin; i++) w[i] = pw_pack_trunc_w((wt8_t)lcg_i8());
        for (int i = 0; i < MAC_PR * MAC_PC * cin; i++) patch[i] = pw_pack_trunc_a((act8_t)lcg_i8());
        fails += !run_case("A_synth_ncbase1", cin, cout, lr, lc, w.data(), patch.data());
    }

    // B: real layer_0051_pwconv (SE fc1), cin=48 cout=768, h=w=1
    {
        int cin = 48, cout = 768, lr = 1, lc = 1;
        auto wbytes = read_file("E:/codes/microzed/fastvit_hls/weights_t8_calibrated_256/layer_0051_pwconv_weight.bin");
        if ((int)wbytes.size() != cout * cin) { fprintf(stderr, "B: weight size mismatch %d vs %d\n", (int)wbytes.size(), cout*cin); return 2; }
        auto abytes = read_file("E:/codes/microzed/fastvit_hls/accuracy_test_imgs_256/entry_77.bin");
        if ((int)abytes.size() != cin) { fprintf(stderr, "B: activation size mismatch %d vs %d\n", (int)abytes.size(), cin); return 2; }
        std::vector<wt4_t> w(cout * cin);
        for (int i = 0; i < cout * cin; i++) w[i] = pw_pack_trunc_w((wt8_t)(int8_t)wbytes[i]);
        std::vector<act5_t> patch(MAC_PR * MAC_PC * cin, act5_t(0));
        for (int ci = 0; ci < cin; ci++)
            patch[(0 * MAC_PC + 0) * cin + ci] = pw_pack_trunc_a((act8_t)(int8_t)abytes[ci]);
        fails += !run_case("B_real_SEfc1_layer0051", cin, cout, lr, lc, w.data(), patch.data());
    }

    // C: real layer_0050_pwconv (SE fc2), cin=768 cout=48, h=w=1
    {
        int cin = 768, cout = 48, lr = 1, lc = 1;
        auto wbytes = read_file("E:/codes/microzed/fastvit_hls/weights_t8_calibrated_256/layer_0050_pwconv_weight.bin");
        if ((int)wbytes.size() != cout * cin) { fprintf(stderr, "C: weight size mismatch %d vs %d\n", (int)wbytes.size(), cout*cin); return 2; }
        auto abytes = read_file("E:/codes/microzed/fastvit_hls/accuracy_test_imgs_256/entry_75.bin");
        if ((int)abytes.size() != cin) { fprintf(stderr, "C: activation size mismatch %d vs %d\n", (int)abytes.size(), cin); return 2; }
        std::vector<wt4_t> w(cout * cin);
        for (int i = 0; i < cout * cin; i++) w[i] = pw_pack_trunc_w((wt8_t)(int8_t)wbytes[i]);
        std::vector<act5_t> patch(MAC_PR * MAC_PC * cin, act5_t(0));
        for (int ci = 0; ci < cin; ci++)
            patch[(0 * MAC_PC + 0) * cin + ci] = pw_pack_trunc_a((act8_t)(int8_t)abytes[ci]);
        fails += !run_case("C_real_SEfc2_layer0050", cin, cout, lr, lc, w.data(), patch.data());
    }

    // D: real layer_0006_pwconv weight, cin=144 cout=48, full spatial tile
    {
        int cin = 144, cout = 48, lr = MAC_PR, lc = MAC_PC;
        auto wbytes = read_file("E:/codes/microzed/fastvit_hls/weights_t8_calibrated_256/layer_0006_pwconv_weight.bin");
        if ((int)wbytes.size() != cout * cin) { fprintf(stderr, "D: weight size mismatch %d vs %d\n", (int)wbytes.size(), cout*cin); return 2; }
        std::vector<wt4_t> w(cout * cin);
        for (int i = 0; i < cout * cin; i++) w[i] = pw_pack_trunc_w((wt8_t)(int8_t)wbytes[i]);
        std::vector<act5_t> patch(MAC_PR * MAC_PC * cin);
        for (int i = 0; i < MAC_PR * MAC_PC * cin; i++) patch[i] = pw_pack_trunc_a((act8_t)lcg_i8());
        fails += !run_case("D_real_w_layer0006_fulltile", cin, cout, lr, lc, w.data(), patch.data());
    }

    printf(">>> %d/4 cases failed\n", fails);
    return fails != 0;
}
