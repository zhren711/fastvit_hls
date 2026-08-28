// dw_linebuf_probe2.cpp -- see dw_linebuf_probe2.h for design rationale.
//
// Window/weight anchoring: the row-buffer shift chain always pushes the
// newest pixel into the BOTTOM-RIGHT corner of window[MAX_K][MAX_K]
// (matching v5's own chain direction), so for real K < MAX_K the ACTIVE
// K x K region is the bottom-right sub-block, window[MAX_K-K+r][MAX_K-K+c]
// for r,c in [0,K) -- not the top-left. Weight is staged into the SAME
// bottom-right-anchored position (WT_STAGE below) specifically so the MAC
// reduction can read window[r][c]*weight[r][c] with the SAME (r,c) for
// both operands, gated by one shared `active` flag -- avoids ever forming
// a runtime-derived index (round-12's mistake) by keeping both arrays'
// index spaces aligned instead of remapping one at MAC time.
#include "dw_linebuf_probe2.h"

// ZHR-92 Phase 1 completion (2026-08-28): weight_aligned used to be a
// separate DATAFLOW task's output (dw_linebuf2_wt_stage), consumed only by
// dw_linebuf2_consume. Being a fully-partitioned array crossing a DATAFLOW
// task boundary, HLS channelized it into 49 independent 2-deep FIFOs
// (~4,851 FF, most of the probe's 3,536 FIFO LUT) -- a stage-boundary
// artifact, not a cost of the alignment computation itself (49 adds/writes,
// trivially cheap on its own). Fixed by inlining the alignment as a
// PROLOGUE inside dw_linebuf2_consume -- weight_aligned becomes a plain
// LOCAL array (same treatment as `window` already gets inside produce),
// never crossing a DATAFLOW channel. This does NOT touch the real
// DATAFLOW structure (produce<->consume still communicate purely via the
// `taps` stream, unchanged) -- this project has broken DATAFLOW three
// times before by restructuring the actual pipelined channel; this fix
// only removes a third task whose sole consumer already fully owns it.
static void dw_linebuf2_produce(
    const act_t pixels[MAC_PD][MAX_PIXELS],
    int h_in, int w_in, int K, int S,
    hls::stream<lb_beat_t> &taps)
{
#pragma HLS ARRAY_PARTITION variable=pixels complete dim=1

    act_t lb0[MAC_PD][WPAD_MAX], lb1[MAC_PD][WPAD_MAX], lb2[MAC_PD][WPAD_MAX];
    act_t lb3[MAC_PD][WPAD_MAX], lb4[MAC_PD][WPAD_MAX], lb5[MAC_PD][WPAD_MAX];
#pragma HLS ARRAY_PARTITION variable=lb0 complete dim=1
#pragma HLS ARRAY_PARTITION variable=lb1 complete dim=1
#pragma HLS ARRAY_PARTITION variable=lb2 complete dim=1
#pragma HLS ARRAY_PARTITION variable=lb3 complete dim=1
#pragma HLS ARRAY_PARTITION variable=lb4 complete dim=1
#pragma HLS ARRAY_PARTITION variable=lb5 complete dim=1
    // column dim (WPAD_MAX) deliberately NOT partitioned -- one real BRAM
    // per channel per row buffer, addressed by pcol (a genuine loop
    // induction variable), same as v5.

    act_t window[MAC_PD][MAX_K][MAX_K];
#pragma HLS ARRAY_PARTITION variable=window complete dim=0

    const int pad   = K / 2;
    const int h_pad = h_in + 2 * pad;
    const int w_pad = w_in + 2 * pad;
    const int off   = MAX_K - K;

    int read_ptr = 0;      /* loop-carried +1 accumulator, replaces real_row*w_in+real_col */
    int row_phase = 0;     /* wrap-pair counter, replaces raw_out_row % S */

    ROW: for (int prow = 0; prow < h_pad; prow++) {
        int real_row = prow - pad;
        bool row_in_image = (real_row >= 0) && (real_row < h_in);

        bool row_valid;
        if (prow < K - 1) {
            row_valid = false;
        } else if (prow == K - 1) {
            row_phase = 0;
            row_valid = true;
        } else {
            row_phase++;
            if (row_phase == S) row_phase = 0;
            row_valid = (row_phase == 0);
        }

        int col_phase = 0;  /* reset each row -- replaces raw_out_col % S */

        COL: for (int pcol = 0; pcol < w_pad; pcol++) {
#pragma HLS PIPELINE II=1
            int real_col = pcol - pad;
            bool col_in_image = row_in_image && (real_col >= 0) && (real_col < w_in);

            bool col_valid;
            if (pcol < K - 1) {
                col_valid = false;
            } else if (pcol == K - 1) {
                col_phase = 0;
                col_valid = true;
            } else {
                col_phase++;
                if (col_phase == S) col_phase = 0;
                col_valid = (col_phase == 0);
            }

            lb_beat_t beat;
            beat.valid = row_valid && col_valid;

            for (int dd = 0; dd < MAC_PD; dd++) {
#pragma HLS UNROLL
                act_t new_pixel = col_in_image ? pixels[dd][read_ptr] : (act_t)0;

                act_t v5 = lb5[dd][pcol]; lb5[dd][pcol] = new_pixel;
                act_t v4 = lb4[dd][pcol]; lb4[dd][pcol] = v5;
                act_t v3 = lb3[dd][pcol]; lb3[dd][pcol] = v4;
                act_t v2 = lb2[dd][pcol]; lb2[dd][pcol] = v3;
                act_t v1 = lb1[dd][pcol]; lb1[dd][pcol] = v2;
                act_t v0 = lb0[dd][pcol]; lb0[dd][pcol] = v1;

                for (int r = 0; r < MAX_K; r++) {
#pragma HLS UNROLL
                    for (int c = 0; c < MAX_K - 1; c++) {
#pragma HLS UNROLL
                        window[dd][r][c] = window[dd][r][c + 1];
                    }
                }
                window[dd][0][MAX_K - 1] = v0;
                window[dd][1][MAX_K - 1] = v1;
                window[dd][2][MAX_K - 1] = v2;
                window[dd][3][MAX_K - 1] = v3;
                window[dd][4][MAX_K - 1] = v4;
                window[dd][5][MAX_K - 1] = v5;
                window[dd][6][MAX_K - 1] = new_pixel;

                for (int r = 0; r < MAX_K; r++) {
#pragma HLS UNROLL
                    for (int c = 0; c < MAX_K; c++) {
#pragma HLS UNROLL
                        beat.window[dd][r][c] = window[dd][r][c];
                    }
                }
            }
            taps.write(beat);

            if (col_in_image) read_ptr++;
        }
    }
    (void)off;
}

static void dw_linebuf2_consume(
    const wt_t weight_in[MAC_PD][MAX_K][MAX_K],  // top-left anchored, raw
    int K,
    int total_beats,
    hls::stream<lb_beat_t> &taps,
    acc_t out[MAC_PD][MAX_OUT],
    int *out_count)
{
    wt_t weight_aligned[MAC_PD][MAX_K][MAX_K];    // LOCAL now -- no DATAFLOW channel
#pragma HLS ARRAY_PARTITION variable=weight_aligned complete dim=0

    const int off = MAX_K - K;
    WT_ALIGN: for (int kh = 0; kh < K; kh++) {
        for (int kw = 0; kw < K; kw++) {
#pragma HLS PIPELINE II=1
            for (int dd = 0; dd < MAC_PD; dd++) {
#pragma HLS UNROLL
                weight_aligned[dd][off + kh][off + kw] = weight_in[dd][kh][kw];
            }
        }
    }

    int idx = 0;
    CONSUME: for (int t = 0; t < total_beats; t++) {
#pragma HLS PIPELINE II=1
        lb_beat_t beat = taps.read();
        for (int dd = 0; dd < MAC_PD; dd++) {
#pragma HLS UNROLL
            acc_t sum = 0;
            for (int r = 0; r < MAX_K; r++) {
#pragma HLS UNROLL
                for (int c = 0; c < MAX_K; c++) {
#pragma HLS UNROLL
                    bool active = (r >= off) && (c >= off);
                    acc_t prod;
                    // ZHR-92 Phase 1 (2026-08-28): toggled via -DLB_FORCE_DSP
                    // for the MAC_PD x {LUT,DSP} budget sweep -- see the
                    // Phase-1 completion report for why this can't be
                    // bound unconditionally (49*MAC_PD DSPs vs 220 total).
#ifdef LB_FORCE_DSP
#pragma HLS BIND_OP variable=prod op=mul impl=DSP
#endif
                    prod = (acc_t)beat.window[dd][r][c] * (acc_t)weight_aligned[dd][r][c];
                    acc_t contrib = active ? prod : (acc_t)0;
                    sum += contrib;
                }
            }
            if (beat.valid && idx < MAX_OUT) {
                out[dd][idx] = sum;
            }
        }
        if (beat.valid) idx++;
    }
    *out_count = idx;
}

void dw_linebuf_probe2(
    const act_t pixels[MAC_PD][MAX_PIXELS],
    const wt_t  weight[MAC_PD][MAX_K][MAX_K],
    acc_t out[MAC_PD][MAX_OUT],
    int h_in, int w_in, int K, int S,
    int *out_count)
{
#pragma HLS ARRAY_PARTITION variable=pixels complete dim=1
#pragma HLS ARRAY_PARTITION variable=weight complete dim=0
#pragma HLS DATAFLOW

    hls::stream<lb_beat_t> taps;
#pragma HLS STREAM variable=taps depth=2

    const int pad   = K / 2;
    const int h_pad = h_in + 2 * pad;
    const int w_pad = w_in + 2 * pad;
    const int total_beats = h_pad * w_pad;

    dw_linebuf2_produce(pixels, h_in, w_in, K, S, taps);
    dw_linebuf2_consume(weight, K, total_beats, taps, out, out_count);
}

// ZHR-92 Phase 1 Step 1 (2026-08-28): fixed-shape wrapper, H_IN/W_IN/K/S
// hardcoded as compile-time literals (real layer_0003_dwconv shape,
// K3S1) instead of runtime function args -- the only change from
// dw_linebuf_probe2() above. Exists solely so csynth can resolve ROW/COL/
// CONSUME's trip counts to real numbers instead of "?", letting this
// round independently measure real cycles/pixel (the one un-verified
// input in the Phase-1 tile-vs-raster cost comparison) instead of taking
// it on faith. Calls the SAME produce/consume as the real (runtime-
// shaped) function -- no separate mechanism, only the call-site literals
// differ.
void dw_linebuf_probe2_fixed(
    const act_t pixels[MAC_PD][MAX_PIXELS],
    const wt_t  weight[MAC_PD][MAX_K][MAX_K],
    acc_t out[MAC_PD][MAX_OUT],
    int *out_count)
{
#pragma HLS ARRAY_PARTITION variable=pixels complete dim=1
#pragma HLS ARRAY_PARTITION variable=weight complete dim=0
#pragma HLS DATAFLOW

    hls::stream<lb_beat_t> taps;
#pragma HLS STREAM variable=taps depth=2

    const int H_IN = 64, W_IN = 64, K = 3, S = 1;
    const int pad   = K / 2;
    const int h_pad = H_IN + 2 * pad;
    const int w_pad = W_IN + 2 * pad;
    const int total_beats = h_pad * w_pad;

    dw_linebuf2_produce(pixels, H_IN, W_IN, K, S, taps);
    dw_linebuf2_consume(weight, K, total_beats, taps, out, out_count);
}
