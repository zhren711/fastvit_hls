// dw_linebuf_real_raster.cpp -- ZHR-92 Phase 1 Step 1 (2026-08-28): runs
// the raster/line-buffer mechanism (dw_linebuf_probe2) on the same real
// per-layer bytes dw_linebuf_real_tile.cpp used, dumps the RAW (pre-bias,
// pre-shift) accumulator sums to dw_linebuf_realdata/<tag>/raster_raw.bin
// (int32 per element) for a separate compare step to apply the real
// bias/shift/clamp and diff against tile_out.bin. Own translation unit
// (dw_linebuf_probe2.h only, -DMAC_PD=48) so mac_array.h's unconditional
// MAC_PD=1 define can't collide with this file's MAC_PD=48.
#include "dw_linebuf_probe2.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <fstream>

#if MAC_PD != 48
#error "this harness assumes MAC_PD=48 (real cin shared by all 4 test layers)"
#endif

static std::vector<unsigned char> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "FATAL: cannot open %s\n", path.c_str()); exit(2); }
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static act_t pixels[MAC_PD][MAX_PIXELS];
static wt_t  weight[MAC_PD][MAX_K][MAX_K];
static acc_t raw_out[MAC_PD][MAX_OUT];

static const char *ROOT_DIR = "E:/codes/microzed/fastvit_hls/fastvit_ip_v2/dw_linebuf_realdata";

static void run_case(const char *tag, int cin, int h_in, int w_in, int k, int stride, int fpg) {
    std::string dir = std::string(ROOT_DIR) + "/" + tag;
    auto in_bytes = read_file(dir + "/in.bin");
    auto w_bytes  = read_file(dir + "/w.bin");

    for (int ci = 0; ci < cin; ci++) {
        for (int i = 0; i < h_in * w_in; i++) pixels[ci][i] = act_t((int8_t)in_bytes[ci * h_in * w_in + i]);
        int co = ci * fpg + 0;  // g=0 real kernel for this input channel; fpg itself out of scope
        for (int r = 0; r < MAX_K; r++)
            for (int c = 0; c < MAX_K; c++)
                weight[ci][r][c] = (r < k && c < k) ? wt_t((int8_t)w_bytes[(size_t)co * k * k + r * k + c]) : wt_t(0);
    }

    int out_count = 0;
    dw_linebuf_probe2(pixels, weight, raw_out, h_in, w_in, k, stride, &out_count);

    std::vector<int32_t> flat(cin * out_count);
    for (int ci = 0; ci < cin; ci++)
        for (int i = 0; i < out_count; i++)
            flat[ci * out_count + i] = (int32_t)raw_out[ci][i];

    std::ofstream of(dir + "/raster_raw.bin", std::ios::binary);
    of.write(reinterpret_cast<const char*>(flat.data()), flat.size() * sizeof(int32_t));

    printf("  [%s] raster_raw written: out_count=%d (cin=%d, %zu int32)\n", tag, out_count, cin, flat.size());
}

int main() {
    printf(">>> raster (dw_linebuf_probe2) run on real bytes, 4 real DW layers\n");
    run_case("K3S1_layer0003", 48,  64,  64, 3, 1, 1);
    run_case("K7S1_layer0004", 48,  64,  64, 7, 1, 1);
    run_case("K3S2_layer0001", 48, 128, 128, 3, 2, 1);
    run_case("K7S2_layer0011", 48,  64,  64, 7, 2, 2);
    printf(">>> done.\n");
    return 0;
}
