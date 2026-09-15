// scalar_ops_real_desc_tb.cpp -- ZHR-92 (2026-09-12): csim coverage for
// GAP / RELU / SIGMOID / SCALE through the real mac_array_top(), using
// the REAL descriptors of the only four real dispatches of these op types
// in the 82-entry network (entries 75/77/79/80: the SE block + the final
// per-channel scale), verbatim from the deployed full-network bundle's
// desc_all.bin (all 28 fields, real in_off/in2_off/out_off into one shared
// arena -- the same single-arena layout the real board driver uses).
//
// Why this exists: until this file, NO csim testbench exercised these four
// ops on the raster architecture (mac_array_tb.cpp's SE phase only pairs
// with the legacy mac_array.cpp; the wiring tb covers DW; gelu_add_burst_tb
// covers GELU/ADD). The SCALAR_OP_SIZE_HOIST round changed all six scalar
// ops' signatures (hw/total hoisted to mac_array_top and passed in), so
// these four were changed-but-never-tested -- and they are live in the
// real network.
//
// Reference: computed INDEPENDENTLY in Python by
// tools/gen_scalar_ops_csim_bundle.py (replicating the hardware's own
// arithmetic -- the placeholder quantized_sigmoid, truncate-toward-zero
// division in GAP, arithmetic-shift clip in SCALE -- not the mathematical
// functions), written as ref_NN.bin. Input is a seeded full-range int8
// PRNG stream (negatives included). The output region is poisoned before
// dispatch so an "IP completes, output silently not written" failure
// (this project's own known defect class) shows up as a mismatch, not a
// pass.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "mac_array.h"

static const char *BUNDLE_DIR = "E:/codes/microzed/fastvit_hls/accuracy_test_imgs_256/csim_scalar_ops";
static const size_t ARENA_BYTES = 2u * 1024u * 1024u;   // real max offset is 1,818,624 + 768
static const act_t POISON = (act_t)0x55;

static bool read_file(const std::string &path, std::vector<char> &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { printf("  !! cannot open %s\n", path.c_str()); return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static const char *op_name(int op)
{
    switch (op) {
        case LDESC_OP_GAP:     return "GAP";
        case LDESC_OP_RELU:    return "RELU";
        case LDESC_OP_SIGMOID: return "SIGMOID";
        case LDESC_OP_SCALE:   return "SCALE";
        default:               return "?";
    }
}

static bool run_entry(int idx)
{
    std::string base = std::string(BUNDLE_DIR) + "/";
    std::vector<char> desc_raw, in_raw, in2_raw, ref_raw;
    if (!read_file(base + "desc_" + std::to_string(idx) + ".bin", desc_raw)) return false;
    if (!read_file(base + "in_"   + std::to_string(idx) + ".bin", in_raw))   return false;
    if (!read_file(base + "ref_"  + std::to_string(idx) + ".bin", ref_raw))  return false;
    if (desc_raw.size() != sizeof(LayerDescV2)) {
        printf("  !! desc_%d.bin is %zu bytes, LayerDescV2 is %zu\n", idx, desc_raw.size(), sizeof(LayerDescV2));
        return false;
    }
    LayerDescV2 d;
    std::memcpy(&d, desc_raw.data(), sizeof(d));

    const int HW    = d.h_in * d.w_in;
    const int total = d.cin * HW;
    if ((int)in_raw.size() != total) {
        printf("  !! in_%d.bin is %zu bytes, expected cin*h*w=%d\n", idx, in_raw.size(), total);
        return false;
    }
    const bool is_scale = (d.op_type == LDESC_OP_SCALE);
    if (is_scale && !read_file(base + "in2_" + std::to_string(idx) + ".bin", in2_raw)) return false;
    const int n_out = (d.op_type == LDESC_OP_GAP) ? d.cin : total;
    if ((int)ref_raw.size() != n_out) {
        printf("  !! ref_%d.bin is %zu bytes, expected %d\n", idx, ref_raw.size(), n_out);
        return false;
    }

    std::vector<act_t> arena(ARENA_BYTES, POISON);
    for (int i = 0; i < total; i++) arena[d.in_off + i] = (act_t)in_raw[i];
    if (is_scale) for (int c = 0; c < d.cin; c++) arena[d.in2_off + c] = (act_t)in2_raw[c];

    int written = 0;
    act_t *A = arena.data();
    mac_array_top(d, A, nullptr, nullptr, A, &written,
                  reinterpret_cast<const ap_uint<32>*>(A),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(A)),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(A)),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(A)),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(A)),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(A)),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(A)));

    int mism = 0, first = -1;
    for (int i = 0; i < n_out; i++) {
        if ((char)arena[d.out_off + i] != ref_raw[i]) { if (first < 0) first = i; mism++; }
    }
    printf("[entry%d %-7s] %s  %d/%d mismatches  (cin=%d h=%d w=%d shift=%d in_off=%d in2_off=%d out_off=%d written=%d)\n",
           idx, op_name(d.op_type), mism ? "FAIL" : "PASS", mism, n_out,
           d.cin, d.h_in, d.w_in, d.out_shift, d.in_off, d.in2_off, d.out_off, written);
    if (mism) {
        printf("  first mismatch at out[%d]: hw=%d ref=%d\n", first,
               (int)(act_t)arena[d.out_off + first], (int)(signed char)ref_raw[first]);
    }
    return mism == 0;
}

int main()
{
    printf(">>> scalar ops (GAP/RELU/SIGMOID/SCALE) via real mac_array_top(), real desc_all.bin descriptors, Python reference\n");
    const int entries[] = {75, 77, 79, 80};
    int failed = 0;
    for (int e : entries) if (!run_entry(e)) failed++;
    printf(">>> %d/4 cases failed\n", failed);
    return failed ? 1 : 0;
}
