// mac_array_raster_integrated_wiring_tb.cpp -- ZHR-92 Phase 1 Step 2 P&R
// round (2026-08-28). Calls the REAL mac_array_top() from
// mac_array_raster_integrated.cpp (DW dispatches internally to
// run_dw_layer_raster via run_layer's early-return branch) for the same
// 5 real DW layers Step 1/2 already validated, and diffs against
// tile_out.bin already saved by the ORIGINAL, untouched mac_array.cpp
// (Step 1's dw_linebuf_realdata/*/tile_out.bin -- known-good real
// hardware-format output). This specifically checks the WIRING in
// run_layer's new early-return branch (are d.cin/d.cout/d.shift_off/etc.
// plumbed correctly into run_dw_layer_raster's call), not the raster
// mechanism itself again (already proven 5/5 in isolation).
#include "mac_array.h"
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
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())));

    auto golden = read_file(dir + "/tile_out.bin");
    if ((int)golden.size() != out_total) {
        printf("[%s] FAIL golden size mismatch: %d vs %d\n", tag, (int)golden.size(), out_total);
        return false;
    }
    int mismatches = 0;
    for (int i = 0; i < out_total; i++) {
        int8_t hw = (int8_t)feat[d.out_off + i];
        int8_t gold = (int8_t)golden[i];
        if (hw != gold) {
            if (mismatches < 5) printf("  [%s] mismatch i=%d integrated=%d golden(orig mac_array.cpp)=%d\n", tag, i, hw, gold);
            mismatches++;
        }
    }
    bool pass = (mismatches == 0);
    printf("[%s] %s  %d/%d mismatches vs original mac_array.cpp's own mac_array_top() output\n",
           tag, pass ? "PASS" : "FAIL", mismatches, out_total);
    return pass;
}

int main() {
    printf(">>> integrated mac_array_top() (DW->run_dw_layer_raster wiring) vs original mac_array.cpp's real output\n");
    int fails = 0;
    fails += !run_case("K3S1_layer0003",       48, 48,  64, 64, 3, 1, 1, 1);
    fails += !run_case("K7S1_layer0004",       48, 48,  64, 64, 7, 1, 3, 1);
    fails += !run_case("K3S2_layer0001",       48, 48, 128,128, 3, 2, 1, 1);
    fails += !run_case("K7S2_layer0011",       48, 96,  64, 64, 7, 2, 3, 2);
    printf(">>> %d/4 cases failed\n", fails);
    return fails != 0;
}
