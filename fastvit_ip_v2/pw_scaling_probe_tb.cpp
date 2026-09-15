/*============================================================
 * pw_scaling_probe_tb.cpp -- ZHR-92 (2026-09-04): csim verification for
 * the 11 synthetic PW scaling-experiment bundles (tools/
 * build_pw_scaling_probes.py) before board testing. Loads each bundle's
 * desc/in/w/b/ref_out.bin (all-ones input/weight, zero bias, out_shift=7
 * trivial golden: out=cin>>7) and checks mac_array_top()'s real output
 * byte-exact against the pre-computed reference.
 *============================================================*/

#include "mac_array.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { printf(">>> FAILED to open %s\n", path.c_str()); exit(1); }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static bool run_case(const char *tag) {
    std::string dir = std::string("E:\\codes\\microzed\\fastvit_hls\\accuracy_test_imgs_256\\board_test_") + tag;

    auto desc_bytes = read_file(dir + "\\desc.bin");
    auto in_bytes   = read_file(dir + "\\in.bin");
    auto w_bytes    = read_file(dir + "\\w.bin");
    auto b_bytes    = read_file(dir + "\\b.bin");
    auto ref_bytes  = read_file(dir + "\\ref_out.bin");

    LayerDescV2 d;
    memcpy(&d, desc_bytes.data(), sizeof(int32_t) * 28);

    /* SEPARATE in/out buffers, matching mac_array_single_op_test.c's own
     * convention (in_v/w_v/b_v/out_v are distinct fixed DRAM offsets on
     * the board, NOT one shared buffer) -- in_off=out_off=0 in these
     * bundles is only alias-free under that convention. Using ONE shared
     * buffer here (as pw_weight_hoist_tb.cpp does, with a nonzero out_off)
     * would corrupt input while writing output -- confirmed the hard way
     * this round (4/11 false FAILs traced to exactly this). */
    std::vector<act_t> in_buf(in_bytes.size());
    for (size_t i = 0; i < in_bytes.size(); i++) in_buf[i] = act_t((int8_t)in_bytes[i]);

    std::vector<act_t> out_buf(ref_bytes.size(), act_t(0));

    std::vector<wt_t> wbuf(w_bytes.size());
    for (size_t i = 0; i < w_bytes.size(); i++) wbuf[i] = wt_t((int8_t)w_bytes[i]);

    std::vector<acc_t> bbuf(b_bytes.size() / 4);
    memcpy(bbuf.data(), b_bytes.data(), b_bytes.size());

    int written[1] = {0};
    mac_array_top(d, in_buf.data(), wbuf.data(), bbuf.data(), out_buf.data(), written,
                  reinterpret_cast<const ap_uint<32>*>(in_buf.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(out_buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(in_buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(in_buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(in_buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(in_buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(wbuf.data())));

    int mismatches = 0;
    for (size_t i = 0; i < ref_bytes.size(); i++)
        if ((int8_t)out_buf[i] != (int8_t)ref_bytes[i]) mismatches++;

    bool ok = (mismatches == 0) && (written[0] == 1);
    printf("[%s] written=%d: %s (%d/%zu mismatches)\n", tag, written[0],
           ok ? "PASS" : "FAIL", mismatches, ref_bytes.size());
    return ok;
}

int main() {
    const char *tags[] = {
        "pwscale_1a_ot48", "pwscale_1b_ot96", "pwscale_1c_ot192",
        "pwscale_2a_cb1", "pwscale_2b_cb2", "pwscale_2c_cb4",
        "pwscale_3a_t16", "pwscale_3b_t64",
        "pwscale_4a_nc1", "pwscale_4b_nc2", "pwscale_4c_nc3",
        "pwaxi_1x", "pwaxi_2x", "pwaxi_4x",
        "pwburst_a1_nw16", "pwburst_a3_nw4", "pwburst_a4_nw2", "pwburst_a5_nw1",
        "pwburst_b1_t16", "pwburst_b3_t256",
    };
    int fails = 0;
    for (const char *t : tags) fails += !run_case(t);
    printf(">>> %d/%d cases failed\n", fails, (int)(sizeof(tags)/sizeof(tags[0])));
    return fails ? 1 : 0;
}
