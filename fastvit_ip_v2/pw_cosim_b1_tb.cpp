/*============================================================
 * pw_cosim_b1_tb.cpp -- ZHR-92 (2026-09-04): minimal single-call
 * testbench for RTL cosimulation, targeting Group B's smallest shape
 * (pwburst_b1_t16: cin=48, cout=48, h=8, w=32, n_tiles=16, n_chunks=1).
 * ONE call to mac_array_top() -- cosim is RTL simulation, real
 * simulation time, so this stays as small as possible while still being
 * a real, board-verified-correct shape (not synthetic-for-cosim-only).
 * Board-measured: 2.19ms (219,000 cycles @ 100MHz); analytical PW_FLAT
 * compute-only prediction: 0.6144ms (61,440 cycles); remainder 1.576ms
 * (71.9% of measured) is the open question this cosim run answers --
 * does the SAME gap appear in cycle-accurate RTL simulation (an RTL-
 * internal cause), or does cosim's idealized AXI model show only the
 * analytical ~61,440 cycles (a real-hardware-only cause: DRAM
 * controller/SmartConnect/PS-side, invisible to RTL simulation)?
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

int main() {
    std::string dir = "E:\\codes\\microzed\\fastvit_hls\\accuracy_test_imgs_256\\board_test_pwburst_b1_t16";

    auto desc_bytes = read_file(dir + "\\desc.bin");
    auto in_bytes   = read_file(dir + "\\in.bin");
    auto w_bytes    = read_file(dir + "\\w.bin");
    auto b_bytes    = read_file(dir + "\\b.bin");
    auto ref_bytes  = read_file(dir + "\\ref_out.bin");

    LayerDescV2 d;
    memcpy(&d, desc_bytes.data(), sizeof(int32_t) * 28);

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
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(in_buf.data())));

    int mismatches = 0;
    for (size_t i = 0; i < ref_bytes.size(); i++)
        if ((int8_t)out_buf[i] != (int8_t)ref_bytes[i]) mismatches++;

    bool ok = (mismatches == 0) && (written[0] == 1);
    printf(">>> pwburst_b1_t16: written=%d: %s (%d/%zu mismatches)\n", written[0],
           ok ? "PASS" : "FAIL", mismatches, ref_bytes.size());
    return ok ? 0 : 1;
}
