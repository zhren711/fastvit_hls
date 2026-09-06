// gelu_add_burst_tb.cpp -- ZHR-92 (2026-09-06): ELEMWISE_BURST correctness
// check. No existing testbench in this codebase dispatches GELU/ADD through
// mac_array_top() against the CURRENT raster architecture (mac_array_tb.cpp
// is the legacy, stale-paired 17-phase suite that aborts at Phase0 against
// this file; mac_array_raster_integrated_wiring_tb.cpp only covers DW) --
// per this project's own repeated pairing-confusion lesson, this is a new,
// dedicated, self-contained testbench rather than reusing either.
// Reference values are computed here by replicating run_gelu/run_add's own
// exact fixed-point arithmetic (quantized_sigmoid, clip_shift) -- both are
// small, deterministic, already-known functions, no external golden file
// needed. Covers: GELU and ADD, several sizes spanning 1 chunk (<4096
// elements) and multiple chunks (>4096, exercising the new ELEMWISE_CHUNK
// outer loop and its uneven-last-chunk case).
#include "mac_array.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>

static int8_t quantized_sigmoid_ref(int8_t x) {
    int32_t v = (int32_t)x + 64;
    if (v > 127) v = 127;
    if (v < 0)   v = 0;
    return (int8_t)v;
}

static int8_t clip_shift_ref(int32_t acc, int shift) {
    int32_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return (int8_t)v;
}

static bool run_gelu_case(const char *tag, int total, int out_shift) {
    std::vector<act_t> buf(2 * total, act_t(0));
    for (int i = 0; i < total; i++) buf[i] = act_t((int8_t)((i * 37 - 91) & 0xFF));

    LayerDescV2 d{};
    d.op_type = LDESC_OP_GELU;
    d.cin = 1; d.h_in = 1; d.w_in = total;
    d.in_off = 0;
    d.out_off = total;
    d.out_shift = out_shift;

    int written = 0;
    mac_array_top(d, buf.data(), nullptr, nullptr, buf.data(), &written,
                  reinterpret_cast<const ap_uint<32>*>(buf.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(buf.data())),
                  hls::burst_maxi<act_t>(buf.data()),
                  hls::burst_maxi<act_t>(buf.data()));

    int mismatches = 0;
    for (int i = 0; i < total; i++) {
        int8_t x = (int8_t)buf[i];
        int8_t sig = quantized_sigmoid_ref(x);
        int32_t prod = (int32_t)x * (int32_t)sig;
        int8_t expect = clip_shift_ref(prod, out_shift);
        int8_t got = (int8_t)buf[d.out_off + i];
        if (got != expect) {
            if (mismatches < 5) printf("  [%s] GELU mismatch i=%d got=%d expect=%d\n", tag, i, got, expect);
            mismatches++;
        }
    }
    bool pass = (mismatches == 0);
    printf("[%s] GELU total=%d %s  %d/%d mismatches\n", tag, total, pass ? "PASS" : "FAIL", mismatches, total);
    return pass;
}

static bool run_add_case(const char *tag, int total, int out_shift) {
    std::vector<act_t> buf(3 * total, act_t(0));
    for (int i = 0; i < total; i++) {
        buf[i] = act_t((int8_t)((i * 53 - 61) & 0xFF));
        buf[total + i] = act_t((int8_t)((i * 17 + 29) & 0xFF));
    }

    LayerDescV2 d{};
    d.op_type = LDESC_OP_ADD;
    d.cin = 1; d.h_in = 1; d.w_in = total;
    d.in_off = 0;
    d.in2_off = total;
    d.out_off = 2 * total;
    d.out_shift = out_shift;

    int written = 0;
    mac_array_top(d, buf.data(), nullptr, nullptr, buf.data(), &written,
                  reinterpret_cast<const ap_uint<32>*>(buf.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(buf.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(buf.data())),
                  hls::burst_maxi<act_t>(buf.data()),
                  hls::burst_maxi<act_t>(buf.data()));

    int mismatches = 0;
    for (int i = 0; i < total; i++) {
        int8_t a = (int8_t)buf[i];
        int8_t b = (int8_t)buf[total + i];
        int32_t sum = (int32_t)a + (int32_t)b;
        int8_t expect = clip_shift_ref(sum, out_shift);
        int8_t got = (int8_t)buf[d.out_off + i];
        if (got != expect) {
            if (mismatches < 5) printf("  [%s] ADD mismatch i=%d got=%d expect=%d\n", tag, i, got, expect);
            mismatches++;
        }
    }
    bool pass = (mismatches == 0);
    printf("[%s] ADD total=%d %s  %d/%d mismatches\n", tag, total, pass ? "PASS" : "FAIL", mismatches, total);
    return pass;
}

int main() {
    printf(">>> ELEMWISE_BURST correctness: GELU/ADD via real mac_array_top(), reference computed in-tb\n");
    int fails = 0;

    // Small: well under one ELEMWISE_CHUNK (4096).
    fails += !run_gelu_case("gelu_small", 100, 0);
    fails += !run_add_case ("add_small",  100, 0);

    // Exactly one chunk boundary.
    fails += !run_gelu_case("gelu_exact_chunk", ELEMWISE_CHUNK, 1);
    fails += !run_add_case ("add_exact_chunk",  ELEMWISE_CHUNK, 1);

    // Multi-chunk, even split.
    fails += !run_gelu_case("gelu_multi_even", ELEMWISE_CHUNK * 3, 2);
    fails += !run_add_case ("add_multi_even",  ELEMWISE_CHUNK * 3, 2);

    // Multi-chunk, uneven last chunk (the real risk case for the chunking loop).
    fails += !run_gelu_case("gelu_multi_uneven", ELEMWISE_CHUNK * 2 + 137, 3);
    fails += !run_add_case ("add_multi_uneven",  ELEMWISE_CHUNK * 2 + 137, 3);

    // A real network-scale case (layer 0's own GELU shape: 48*128*128).
    fails += !run_gelu_case("gelu_real_layer0", 48 * 128 * 128, 4);

    printf(">>> %d/8 cases failed\n", fails);
    return fails != 0;
}
