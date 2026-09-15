// dw_raster_layer_tb.cpp -- ZHR-92 Phase 1 Step 2 (2026-08-28): csim
// correctness check for run_dw_layer_raster() (the real, descriptor-
// driven raster mechanism, fpg included) against the CURRENT tile-based
// mac_array_top() -- both run on the exact same real per-layer bytes.
// 5 real layers: the 3 fpg=1 combos already validated in Step 1
// (regression -- confirms the move from probe-style to real-descriptor
// integration didn't break anything) plus 2 new fpg=2 layers
// (layer_0011 stride=2, layer_0049/FinalDW stride=1 -- the two fpg=2
// sub-cases flagged as needing separate coverage).
#include "mac_array.h"
#include "dw_raster_layer.h"
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

static const char *ROOT_DIR = "E:/codes/microzed/fastvit_hls/fastvit_ip_v2/dw_linebuf_realdata";

static bool run_case(const char *tag, int cin, int cout, int h_in, int w_in,
                      int k, int stride, int pad, int fpg) {
    std::string dir = std::string(ROOT_DIR) + "/" + tag;
    auto in_bytes    = read_file(dir + "/in.bin");
    auto w_bytes     = read_file(dir + "/w.bin");
    auto b_bytes     = read_file(dir + "/b.bin");
    auto shift_bytes = read_file(dir + "/shift.bin");

    // ---- (a) real tile-based mac_array_top() ----
    LayerDescV2 d{};
    d.op_type = LDESC_OP_DWCONV;
    d.cin = cin; d.cout = cout; d.h_in = h_in; d.w_in = w_in;
    d.k = k; d.stride = stride; d.pad = pad; d.fpg = fpg;
    d.out_shift = 0;
    d.in_off = 0; d.w_off = 0; d.b_off = 0;
    d.in2_off = 0;
    d.use_shift_table = 1; d.shift_off = (int)w_bytes.size();

    MacArrayParams p = derive_mac_array_params(d);
    d.h_out = p.h_out; d.w_out = p.w_out;
    d.in_ch_stride = p.in_ch_stride; d.out_ch_stride = p.out_ch_stride;
    d.n_row_tiles = p.n_row_tiles; d.n_col_tiles = p.n_col_tiles; d.n_ch_tiles = p.n_ch_tiles;
    d.last_row_tile = p.last_row_tile; d.last_col_tile = p.last_col_tile; d.last_ch_tile = p.last_ch_tile;

    int in_total = cin * h_in * w_in;
    int out_total = cout * p.h_out * p.w_out;
    d.out_off = in_total;

    std::vector<act_t> feat(in_total + out_total, act_t(0));
    for (int i = 0; i < in_total; i++) feat[i] = act_t((int8_t)in_bytes[i]);

    std::vector<wt_t> wbuf(w_bytes.size() + shift_bytes.size(), wt_t(0));
    for (size_t i = 0; i < w_bytes.size(); i++) wbuf[i] = wt_t((int8_t)w_bytes[i]);
    for (size_t i = 0; i < shift_bytes.size(); i++) wbuf[w_bytes.size() + i] = wt_t((int8_t)shift_bytes[i]);

    std::vector<acc_t> bbuf(cout, acc_t(0));
    for (int c = 0; c < cout; c++) {
        int32_t bv;
        memcpy(&bv, &b_bytes[c * 4], 4);
        bbuf[c] = acc_t(bv);
    }

    int written = 0;
    mac_array_top(d, feat.data(), wbuf.data(), bbuf.data(), feat.data(), &written,
                  reinterpret_cast<const ap_uint<32>*>(feat.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(wbuf.data())));

    // ---- (b) new raster mechanism, real descriptor-shaped call ----
    std::vector<act_t> in_raster(in_total);
    for (int i = 0; i < in_total; i++) in_raster[i] = act_t((int8_t)in_bytes[i]);
    std::vector<wt_t> w_raster(w_bytes.size());
    for (size_t i = 0; i < w_bytes.size(); i++) w_raster[i] = wt_t((int8_t)w_bytes[i]);
    std::vector<wt_t> shift_raster(shift_bytes.size());
    for (size_t i = 0; i < shift_bytes.size(); i++) shift_raster[i] = wt_t((int8_t)shift_bytes[i]);
    std::vector<acc_t> b_raster(cout);
    for (int c = 0; c < cout; c++) b_raster[c] = bbuf[c];
    std::vector<act_t> raster_out(out_total, act_t(0));

    // shift table lives in its own small buffer here (not appended to
    // w_raster) -- run_dw_layer_raster indexes w_base[shift_off + co], so
    // give it a combined buffer just like the real w_base convention.
    std::vector<wt_t> w_combined(w_bytes.size() + shift_bytes.size());
    for (size_t i = 0; i < w_bytes.size(); i++) w_combined[i] = w_raster[i];
    for (size_t i = 0; i < shift_bytes.size(); i++) w_combined[w_bytes.size() + i] = shift_raster[i];

    run_dw_layer_raster(
        in_raster.data(), w_combined.data(), b_raster.data(), raster_out.data(),
        hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(in_raster.data())),
#ifdef DW_OUTPUT_BURST
        hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(raster_out.data())),
#endif
        cin, cout, h_in, w_in, k, stride, pad, fpg,
        /*in_off*/0, /*w_off*/0, /*b_off*/0, /*out_off*/0,
        /*shift_off*/(int)w_bytes.size(),
        p.in_ch_stride, p.out_ch_stride, p.h_out, p.w_out);

    // ---- compare ----
    int mismatches = 0;
    for (int i = 0; i < out_total; i++) {
        act_t hw = feat[d.out_off + i];
        act_t rr = raster_out[i];
        if (hw != rr) {
            if (mismatches < 5)
                printf("  [%s] mismatch i=%d tile=%d raster=%d\n", tag, i, (int)hw, (int)rr);
            mismatches++;
        }
    }
    bool pass = (mismatches == 0);
    printf("[%s] %s  %d/%d mismatches  (cin=%d cout=%d h_in=%d w_in=%d k=%d S=%d fpg=%d)\n",
           tag, pass ? "PASS" : "FAIL", mismatches, out_total, cin, cout, h_in, w_in, k, stride, fpg);
    return pass;
}

int main() {
    printf(">>> run_dw_layer_raster() vs real mac_array_top(), 5 real DW layers (3 regression + 2 new fpg=2)\n");
    int fails = 0;
    fails += !run_case("K3S1_layer0003",       48, 48,  64, 64, 3, 1, 1, 1);
    fails += !run_case("K7S1_layer0004",       48, 48,  64, 64, 7, 1, 3, 1);
    fails += !run_case("K3S2_layer0001",       48, 48, 128,128, 3, 2, 1, 1);
    fails += !run_case("K7S2_layer0011",       48, 96,  64, 64, 7, 2, 3, 2);
    fails += !run_case("K3S1fpg2_layer0049",  384,768,   8,  8, 3, 1, 1, 2);
    printf(">>> %d/5 cases failed\n", fails);
    return fails != 0;
}
