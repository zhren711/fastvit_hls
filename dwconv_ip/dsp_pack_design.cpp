#include "dsp_pack_design.h"

void dsp_packed_mac2_top(act_t fi0, act_t fi1, wt_t w, acc_t &out0, acc_t &out1) {
    ap_int<25> a_shifted = (ap_int<25>)fi1 << 16;
    ap_int<25> d_val     = fi0;
    ap_int<25> sum = a_shifted + d_val;
    ap_int<18> b_packed = w;
    ap_int<43> p = sum * b_packed;
    ap_int<16> slot0 = p.range(15, 0);
    ap_int<16> slot1_raw = p.range(31, 16);
    ap_int<16> slot1 = (slot0 < 0) ? (ap_int<16>)(slot1_raw + 1) : slot1_raw;
    out0 = (acc_t)slot0;
    out1 = (acc_t)slot1;
}
