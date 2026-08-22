/* verify_bundle_entry5_dw.cpp -- ZHR-92, 2026-08-22: isolate "my board
 * test bundle's relocated descriptor is wrong" from "real hardware
 * differs from csim" for the DW board test failure (175,877/196,608
 * mismatches). Runs mac_array_top's REAL csim (same function the board's
 * bitstream was synthesized from) against the EXACT relocated
 * desc.bin/in.bin/w.bin/b.bin the board received, and diffs the result
 * against ref_out.bin. If THIS also mismatches, the bug is in the bundle
 * construction (tools/build_single_op_test_entry5_dw.py), not hardware.
 * If THIS matches, hardware genuinely diverges from csim for DW.
 */
#include "mac_array.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static std::vector<int8_t> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<int8_t> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);
    return buf;
}

int main() {
    const char *dir = "E:\\codes\\microzed\\fastvit_hls\\accuracy_test_imgs_256\\board_test_entry5_dw";
    char path[512];

    snprintf(path, sizeof(path), "%s/desc.bin", dir);
    std::vector<int8_t> desc_raw = read_file(path);
    if (desc_raw.size() != 27 * 4) { fprintf(stderr, "desc.bin wrong size %zu\n", desc_raw.size()); return 1; }
    LayerDescV2 d;
    memcpy(&d, desc_raw.data(), sizeof(int32_t) * 27);

    snprintf(path, sizeof(path), "%s/in.bin", dir);
    std::vector<int8_t> in_buf = read_file(path);
    snprintf(path, sizeof(path), "%s/w.bin", dir);
    std::vector<int8_t> w_buf = read_file(path);
    snprintf(path, sizeof(path), "%s/b.bin", dir);
    std::vector<int8_t> b_raw = read_file(path);
    snprintf(path, sizeof(path), "%s/ref_out.bin", dir);
    std::vector<int8_t> ref = read_file(path);

    printf(">>> desc: op_type=%d cin=%d cout=%d h_in=%d w_in=%d k=%d stride=%d pad=%d fpg=%d\n",
           d.op_type, d.cin, d.cout, d.h_in, d.w_in, d.k, d.stride, d.pad, d.fpg);
    printf(">>> in_off=%d w_off=%d b_off=%d out_off=%d use_shift_table=%d shift_off=%d\n",
           d.in_off, d.w_off, d.b_off, d.out_off, d.use_shift_table, d.shift_off);
    printf(">>> h_out=%d w_out=%d in_ch_stride=%d out_ch_stride=%d\n",
           d.h_out, d.w_out, d.in_ch_stride, d.out_ch_stride);

    int b_elems = (int)(b_raw.size() / 4);
    std::vector<acc_t> b_buf(b_elems);
    for (int i = 0; i < b_elems; i++) {
        int32_t v;
        memcpy(&v, b_raw.data() + i * 4, 4);
        b_buf[i] = acc_t(v);
    }

    /* Board reality: in_base and out_base are SEPARATE physical regions
     * (different pointers), each with in_off/out_off=0 relative to its
     * OWN base -- NOT one shared array where in_off/out_off alias. Using
     * one shared buffer here would corrupt the input while writing the
     * output, a bug this tool itself would introduce, not the board. */
    std::vector<act_t> act_in(in_buf.size(), act_t(0));
    for (size_t i = 0; i < in_buf.size(); i++) act_in[i] = act_t(in_buf[i]);
    std::vector<act_t> act_out(ref.size(), act_t(0));

    std::vector<wt_t> w_vec(w_buf.size());
    for (size_t i = 0; i < w_buf.size(); i++) w_vec[i] = wt_t(w_buf[i]);

    int written[1] = {0};
    mac_array_top(&d, 1, act_in.data(), w_vec.data(), b_buf.data(), act_out.data(), written);

    printf(">>> out_written[0] = %d\n", written[0]);

    size_t mism = 0;
    int max_ad = 0;
    for (int i = 0; i < (int)ref.size(); i++) {
        int8_t got = (int8_t)act_out[d.out_off + i];
        int diff = (int)got - (int)ref[i];
        if (diff != 0) mism++;
        int ad = diff < 0 ? -diff : diff;
        if (ad > max_ad) max_ad = ad;
    }
    printf(">>> vs ref_out.bin (csim, using board bundle's relocated desc/data): mismatches = %zu / %zu, max_abs_diff = %d\n",
           mism, ref.size(), max_ad);
    printf(">>> %s\n", mism == 0 ? "PASS -- bundle construction is correct, bug (if any) is real hardware" :
                                     "FAIL -- bundle construction itself is wrong, not a hardware issue");
    return mism == 0 ? 0 : 1;
}
