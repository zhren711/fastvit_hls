// dw_linebuf_real_tile.cpp -- ZHR-92 Phase 1 Step 1 (2026-08-28): runs the
// REAL, current tile-based mac_array_top() on the 4 real (K,S) DW layer
// bundles built by tools/build_dw_linebuf_realdata.py, dumps each layer's
// real int8 output to dw_linebuf_realdata/<tag>/tile_out.bin. Kept in its
// own translation unit (mac_array.h only) so mac_array.h's unconditional
// `#define MAC_PD 1` can't collide with dw_linebuf_probe2.h's own
// MAC_PD=48 override in the raster-side harness.
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

static void run_case(const char *tag, int cin, int cout, int h_in, int w_in,
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

    int written[1] = {0};
    mac_array_top(&d, 1, feat.data(), wbuf.data(), bbuf.data(), feat.data(), written,
                  reinterpret_cast<const ap_uint<32>*>(feat.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())));

    std::vector<int8_t> out_bytes(out_total);
    for (int i = 0; i < out_total; i++) out_bytes[i] = (int8_t)feat[d.out_off + i];

    std::ofstream of(dir + "/tile_out.bin", std::ios::binary);
    of.write(reinterpret_cast<const char*>(out_bytes.data()), out_bytes.size());

    printf("  [%s] tile_out written: %d bytes (cout=%d h_out=%d w_out=%d written=%d)\n",
           tag, out_total, cout, p.h_out, p.w_out, written[0]);
}

int main() {
    printf(">>> real tile-based mac_array_top() run, 4 real DW layers\n");
    run_case("K3S1_layer0003", 48, 48,  64,  64, 3, 1, 1, 1);
    run_case("K7S1_layer0004", 48, 48,  64,  64, 7, 1, 3, 1);
    run_case("K3S2_layer0001", 48, 48, 128, 128, 3, 2, 1, 1);
    run_case("K7S2_layer0011", 48, 96,  64,  64, 7, 2, 3, 2);
    printf(">>> done.\n");
    return 0;
}
