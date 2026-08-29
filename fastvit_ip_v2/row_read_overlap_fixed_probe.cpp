// row_read_overlap_fixed_probe.cpp -- ZHR-92 line-C rescope, Step 4
// (2026-08-29). Determinate-trip-count variant of row_read_produce/
// row_read_consume/row_read_dataflow (mac_array_rowread_overlap_integrated.cpp),
// same methodology as this project's own dw_linebuf_probe2_fixed/
// pw_flat_pipeline_packed_fixed precedent: literal compile-time constants
// in place of runtime LayerDescV2-derived fields, so csynth can resolve a
// real numeric latency instead of "?". Serves two purposes this round:
//   1. Real numeric proof of overlap (or its absence): does
//      row_read_dataflow's own total latency come out close to
//      max(produce, consume) or close to produce+consume.
//   2. A small, fast target for `cosim_design` (RTL C/RTL cosimulation --
//      a LOCAL simulation step using the real generated RTL, distinct
//      from board deployment) to empirically test the HLS 200-656
//      deadlock warning found in Step 3, per Xilinx's own documented
//      recommendation for this exact warning class ("Use the deadlock
//      detection capabilities of RTL cosimulation").
//
// Shape: Cin=8, W=8, H=4 (>= MAC_PR so every rr is in-bounds), r_sz=4
// (full tile, no partial-row zero-fill path), rt=0, in_off=0,
// in_ch_stride=W*H=32 (contiguous channel-major, matching the real
// design's in_ch_stride=h_in*w_in convention). n_words per channel-row =
// ceil((0+8+3)/4) = 2 (byte-aligned, r=0 since in_off=0 and in_ch_stride/
// oh*W are both W-aligned multiples of 4 at this W).
#include "mac_array.h"

#define FIXED_CIN 8
#define FIXED_W 8
#define FIXED_H 4
#define FIXED_R_SZ 4
#define FIXED_RT 0
#define FIXED_IN_OFF 0
#define FIXED_IN_CH_STRIDE (FIXED_W * FIXED_H)

static void row_read_produce_fixed(hls::burst_maxi<ap_uint<32> > &in_burst)
{
    for (int rr = 0; rr < MAC_PR; rr++) {
        int oh = FIXED_RT * MAC_PR + rr;   // FIXED_R_SZ == MAC_PR, every rr valid
        int ch_base = 0;
        ROW_READ_PRODUCE_CH_FIXED: for (int ci = 0; ci < FIXED_CIN; ci++) {
            #pragma HLS PIPELINE II=1
            int byte_addr = FIXED_IN_OFF + ch_base + oh * FIXED_W;
            int word_addr0 = byte_addr >> 2;
            int r = byte_addr & 3;
            int n_words = (r + FIXED_W + 3) >> 2;
            in_burst.read_request((size_t)word_addr0, (unsigned)n_words);
            ch_base += FIXED_IN_CH_STRIDE;
        }
    }
}

static void row_read_consume_fixed(
    hls::burst_maxi<ap_uint<32> > &in_burst,
    act_t row_buf[MAC_PR][MAX_CIN_TIMES_W])
{
    for (int rr = 0; rr < MAC_PR; rr++) {
        int oh = FIXED_RT * MAC_PR + rr;
        int ch_base = 0;
        int flat_base = 0;
        ROW_READ_CONSUME_CH_FIXED: for (int ci = 0; ci < FIXED_CIN; ci++) {
            int byte_addr = FIXED_IN_OFF + ch_base + oh * FIXED_W;
            int r = byte_addr & 3;
            int n_words = (r + FIXED_W + 3) >> 2;
            ROW_READ_CONSUME_FILL_FIXED: for (int i = 0; i < MAX_WORDS_PER_CH; i++) {
                #pragma HLS PIPELINE II=1
                bool word_valid = i < n_words;
                ap_uint<32> wd = word_valid ? in_burst.read() : (ap_uint<32>)0;
                for (int b = 0; b < 4; b++) {
                    #pragma HLS UNROLL
                    int pos = i * 4 + b - r;
                    bool valid = word_valid && (pos >= 0) && (pos < FIXED_W);
                    if (valid) {
                        row_buf[rr][flat_base + pos] = (act_t)wd.range(b * 8 + 7, b * 8);
                    }
                }
            }
            ch_base += FIXED_IN_CH_STRIDE;
            flat_base += FIXED_W;
        }
    }
}

void row_read_dataflow_fixed(
    hls::burst_maxi<ap_uint<32> > in_burst,
    act_t row_buf[MAC_PR][MAX_CIN_TIMES_W])
{
#pragma HLS INTERFACE m_axi port=in_burst offset=slave bundle=gmem_test depth=64
#pragma HLS INTERFACE s_axilite port=in_burst bundle=control
#pragma HLS INTERFACE bram port=row_buf
#pragma HLS INTERFACE s_axilite port=return bundle=control
    #pragma HLS DATAFLOW
    row_read_produce_fixed(in_burst);
    row_read_consume_fixed(in_burst, row_buf);
}
