/*================================================================
 * flat_pipeline_probe.cpp -- A3 run_layer rewrite feasibility spike
 * (ZHR-92, 2026-08-23), stage 1 of the approved design. Proves out PW's
 * flat single-PIPELINE execution model in isolation before touching
 * production code, same pattern as dataflow_probe.cpp. csynth-only --
 * this is about HLS's own structural behavior (achieved II, divide/mod
 * instances, LUT, burst inference), not numeric correctness, which
 * stage 2 verifies via the full csim regression once this is wired into
 * real code. No testbench.
 *
 * Scope, deliberately narrowed for a first spike (see the design doc on
 * ZHR-92):
 *   - PW only, not DW (unification is a later decision).
 *   - One (rt,colt) tile only -- patch[]/bias_cache[] are pre-staged
 *     inputs (same as today's pw_patch_full/pw_bias_cache), matching
 *     the explicit scope decision to flatten (ot,cbase,step), not the
 *     tile boundary.
 *   - Cin assumed an exact multiple of MAX_CIN_PW=32 -- every cbase is a
 *     full 32-wide chunk, n_steps=16 always. No partial-tail-chunk
 *     handling (irrelevant to what this spike tests).
 *   - Full spatial tile only (r_sz==MAC_PR, col_sz==MAC_PC) -- sidesteps
 *     the WRITEOUT boundary-guard/burst question on purpose, to test
 *     the flat pipeline's OWN burst behavior in the ideal case first.
 *
 * Flat counter design is the actual point of this spike: NO division or
 * modulo anywhere, by construction. Every derived index (channel
 * offset, per-ot weight base, output channel base, writeout row/col) is
 * a loop-carried ACCUMULATOR that increments and wraps via plain
 * compare-and-add -- the same technique already proven throughout
 * mac_array.cpp (in_ch_base, wt_base, ot_out_ch_base). Round 13's
 * `kh = step / MAX_K` (2,180 sparsemux cores from one division) is
 * exactly the mistake being avoided. The `k` counter's two sub-uses
 * (0..15 compute step, 0..15 writeout row*4+col) are handled via their
 * own wrap-pair (wr_row/wr_col) rather than idx/4 and idx%4 -- MAC_PC=4
 * being a power of 2 would make those free shifts/masks anyway, but the
 * wrap-pair form removes any ambiguity and matches this file's own
 * established idiom uniformly.
 *
 * acc's cross-iteration accumulation mirrors today's UNIFIED (already
 * proven to achieve II=1) extended across cbase AND phase boundaries --
 * whether II=1 survives those boundary transitions (compute->writeout,
 * writeout->next-ot-compute) is the single biggest open question this
 * spike exists to answer; a lower achieved II is not a failure (see the
 * design doc's stage-1 exit criteria).
 *================================================================*/
#include "mac_array.h"

#define N_STEPS_PER_CBASE 16   /* MAX_CIN_PW/MAC_PD, compile-time constant */
#define N_WRITEOUT_ELEMS  16   /* MAC_PR*MAC_PC, compile-time constant, same value deliberately */

void flat_pipeline_probe_top(
    const wt_t w_base[],
    const act_t patch[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t bias_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    int Cin, int n_cbase, int n_ot, int out_off, int out_ch_stride,
    int total_iters)
{
#pragma HLS INTERFACE m_axi port=w_base    offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=out_base  offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=w_base bundle=control
#pragma HLS INTERFACE s_axilite port=patch bundle=control
#pragma HLS INTERFACE s_axilite port=bias_cache bundle=control
#pragma HLS INTERFACE s_axilite port=out_base bundle=control
#pragma HLS INTERFACE s_axilite port=Cin bundle=control
#pragma HLS INTERFACE s_axilite port=n_cbase bundle=control
#pragma HLS INTERFACE s_axilite port=n_ot bundle=control
#pragma HLS INTERFACE s_axilite port=out_off bundle=control
#pragma HLS INTERFACE s_axilite port=out_ch_stride bundle=control
#pragma HLS INTERFACE s_axilite port=total_iters bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    /* patch/bias_cache are pre-staged ON-CHIP buffers in the real design
     * (pw_patch_full/pw_bias_cache are `static`, not DRAM pointers) --
     * matched here as plain local-style arrays, not a second m_axi
     * master, for a fair LUT comparison against production. */
#pragma HLS ARRAY_PARTITION variable=patch cyclic factor=MAC_PD dim=1
#pragma HLS ARRAY_PARTITION variable=patch complete dim=2
#pragma HLS ARRAY_PARTITION variable=patch complete dim=3

    acc_t acc[MAC_PD][MAC_PR][MAC_PC];
#pragma HLS ARRAY_PARTITION variable=acc complete dim=0

    /* loop-carried state -- all plain counters, wrap via compare+add,
     * no derived multiply/divide/mod anywhere in the hot loop. */
    int k = 0;                  /* 0..15, dual-use: compute step OR writeout elem */
    bool in_writeout = false;
    int cbase_idx = 0;
    int ch_off = 0;              /* channel offset within Cin, shared by patch+weight addressing */
    int w_ot_base = 0;           /* == ot*Cin, accumulated */
    int ot_out_ch_base = 0;      /* == ot*out_ch_stride, accumulated */
    int ot_idx = 0;
    int wr_row = 0, wr_col = 0;  /* writeout row/col -- wrap-pair, not idx/4 and idx%4 */

    FLAT: for (int i = 0; i < total_iters; i++) {
#pragma HLS PIPELINE II=1
        bool reset_acc = (!in_writeout) && (cbase_idx == 0) && (k == 0);

        if (!in_writeout) {
            /* ---- compute step: same body as today's GATHER_ALL_PW +
             * UNIFIED combined into one cycle (no separate gather-then-
             * accumulate phases in a truly flat single-stage pipeline). */
            wt_t lane_w[MAC_PD];
            act_t lane_in[MAC_PD][MAC_PR][MAC_PC];
            #pragma HLS ARRAY_PARTITION variable=lane_w complete dim=0
            #pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                lane_w[dd] = w_base[w_ot_base + ch_off + dd];
                for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        lane_in[dd][rr][cw] = patch[ch_off + dd][rr][cw];
                    }
                }
            }
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        acc_t prod = (acc_t)lane_in[dd][rr][cw] * (acc_t)lane_w[dd];
                        acc[dd][rr][cw] = reset_acc ? prod : (acc_t)(acc[dd][rr][cw] + prod);
                    }
                }
            }
        } else {
            /* ---- writeout element: combine MAC_PD lanes, store one byte.
             * Address is out_off + ot_out_ch_base + wr_row*MAC_PC + wr_col
             * -- monotonically increasing, unconditional every iteration
             * of this phase (unlike WRITEOUT_PW's boundary-guarded form)
             * since this spike deliberately assumes a full tile. */
            acc_t total = 0;
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                total += acc[dd][wr_row][wr_col];
            }
            acc_t biased = total + bias_cache[ot_idx];
            act_t v = (biased > 127) ? (act_t)127 : (biased < -128) ? (act_t)-128 : (act_t)biased;
            out_base[out_off + ot_out_ch_base + wr_row * MAC_PC + wr_col] = v;
        }

        /* ---- counter update, no division/modulo anywhere */
        if (k == N_STEPS_PER_CBASE - 1) {
            k = 0;
            if (!in_writeout) {
                if (cbase_idx == n_cbase - 1) {
                    in_writeout = true;
                } else {
                    cbase_idx++;
                    ch_off += MAX_CIN_PW;
                }
            } else {
                in_writeout = false;
                cbase_idx = 0;
                ch_off = 0;
                w_ot_base += Cin;
                ot_out_ch_base += out_ch_stride;
                ot_idx++;
                wr_row = 0;
                wr_col = 0;
            }
        } else {
            k++;
            if (!in_writeout) {
                ch_off += MAC_PD;
            } else {
                if (wr_col == MAC_PC - 1) { wr_col = 0; wr_row++; }
                else { wr_col++; }
            }
        }
    }
}
