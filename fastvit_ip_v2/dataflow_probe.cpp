/*================================================================
 * dataflow_probe.cpp -- A3 DATAFLOW feasibility probe (ZHR-92,
 * 2026-08-23). Time-boxed ONE round: does #pragma HLS DATAFLOW even
 * apply to PW's three sequential per-cbase stages (weight staging ->
 * gather -> UNIFIED accumulate), and if not, what exactly does it
 * object to? Pure csynth diagnostic -- this file is NEVER included by
 * mac_array_top's own synthesis, has its own separate top function, and
 * is not wired into the production build at all. Not trying to make
 * DATAFLOW pass -- collecting the violation list AS the round's output.
 *
 * Faithfully mirrors the two hazards already suspected in the real code
 * (see mac_array.cpp's PW_WSTAGE/GATHER_ALL_PW/UNIFIED, run_reduce_unified):
 *   1. pw_patch_full is read (not produced) by every cbase iteration --
 *      not written within this region at all, a shared read-only input,
 *      not a single-producer/single-consumer channel between stages.
 *   2. acc accumulates ACROSS cbase iterations -- iteration N's UNIFIED
 *      stage both reads and writes the SAME acc array that iteration
 *      N+1 will also read+write -- a loop-carried true dependency
 *      through the accumulator, not a straight-line producer->consumer
 *      pipe.
 * "PW_STAGE" in the round's framing means PW_WSTAGE (the actual current
 * name) -- the original PW_STAGE activation-copy step this file's
 * history describes was eliminated in an earlier round; the current
 * three sequential per-cbase regions are WSTAGE (weight read) -> GATHER
 * -> UNIFIED, which is what's wrapped here.
 *
 * RESULT (2026-08-23): csynth FAILS outright -- "ERROR: [HLS 200-70]
 * Pre-synthesis failed." One root-cause error class, repeated once per
 * acc scalar (32x, acc is fully partitioned): "HLS 200-976: Argument
 * 'acc_X_Y_Z' failed dataflow checking: Cannot read as well as write
 * over function parameter. Check if the variable used as a channel can
 * be declared inside the dataflow region." This is hazard 2 exactly --
 * acc accumulates across cbase iterations and is a function parameter,
 * not a variable declared inside the region; HLS's canonical-dataflow
 * channel model categorically forbids a cross-iteration read+write on a
 * parameter. The suggested fix (declare it inside the region) is
 * structurally impossible here -- acc must persist across iterations
 * AND be visible to the caller afterward, which is the entire point of
 * it being a parameter. Hazard 1 (pw_patch_full's shared-read-only-
 * across-iterations pattern) did NOT trigger any error -- reading the
 * same array repeatedly across dataflow iterations is fine in this
 * tool's model; only the accumulator pattern was rejected. One other
 * non-fatal warning (HLS 214-114, canonical-form: a plain statement
 * `int c0 = cbase * MAX_CIN_PW;` mixed with declarations/calls) did not
 * cause the failure. DATAFLOW is a dead end for this loop shape as
 * literally applied -- not workaroundable by restructuring
 * pw_patch_full's access pattern, since that was never the problem.
 * Manual double-buffering (or some other explicit overlap mechanism
 * that doesn't route the accumulator through DATAFLOW's channel
 * checking) is the next real candidate. See ZHR-92 for the full
 * decision record.
 *================================================================*/
#include "mac_array.h"

#define N_CBASE_PROBE 5   /* arbitrary >1 so the DATAFLOW'd loop has more
                            * than one real iteration to pipeline across --
                            * matches real shapes like entry9 (n_cbase=5) */

void dataflow_probe_top(
    const wt_t w_base[],
    int w_off, int Cin, int ot,
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    acc_t acc[MAC_PD][MAC_PR][MAC_PC])
{
#pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic factor=MAC_PD dim=1
#pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
#pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3
#pragma HLS ARRAY_PARTITION variable=acc complete dim=0

    CBASE_LOOP: for (int cbase = 0; cbase < N_CBASE_PROBE; cbase++) {
#pragma HLS DATAFLOW
        wt_t pw_wtile[MAX_CIN_PW];
        act_t lane_in_all[16][MAC_PD][MAC_PR][MAC_PC];
        wt_t  lane_w_all[16][MAC_PD];
#pragma HLS ARRAY_PARTITION variable=pw_wtile cyclic factor=MAC_PD dim=1
#pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=2
#pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=3
#pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=4
#pragma HLS ARRAY_PARTITION variable=lane_w_all complete dim=2

        int c0 = cbase * MAX_CIN_PW;

        /* Stage 1 -- was PW_WSTAGE. */
        WSTAGE: for (int ci = 0; ci < MAX_CIN_PW; ci++) {
            pw_wtile[ci] = ((c0 + ci) < Cin)
                ? w_base[w_off + ot * Cin + c0 + ci] : (wt_t)0;
        }

        /* Stage 2 -- was GATHER_ALL_PW. Reads pw_patch_full, a shared
         * input NOT produced within this region -- suspected hazard 1. */
        GATHER: for (int step = 0; step < 16; step++) {
            int cib = step * MAC_PD;
            for (int dd = 0; dd < MAC_PD; dd++) {
                lane_w_all[step][dd] = pw_wtile[cib + dd];
                for (int rr = 0; rr < MAC_PR; rr++) {
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        lane_in_all[step][dd][rr][cw] = pw_patch_full[c0 + cib + dd][rr][cw];
                    }
                }
            }
        }

        /* Stage 3 -- was UNIFIED. acc accumulates ACROSS cbase
         * iterations -- suspected hazard 2 (loop-carried feedback). */
        UNIFIED: for (int step = 0; step < 16; step++) {
#pragma HLS PIPELINE II=1
            for (int dd = 0; dd < MAC_PD; dd++) {
#pragma HLS UNROLL
                for (int rr = 0; rr < MAC_PR; rr++) {
#pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
#pragma HLS UNROLL
                        acc[dd][rr][cw] += (acc_t)lane_in_all[step][dd][rr][cw] * (acc_t)lane_w_all[step][dd];
                    }
                }
            }
        }
    }
}
