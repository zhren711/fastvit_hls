/*================================================================
 * row_hoist_probe.cpp -- ZHR-92 (2026-08-25): combined Phase-1 probe for
 * the PW_PATCH_HOIST row-level burst hoist plan. Tests THREE things in
 * one probe, per instruction -- if any one fails, the whole plan is
 * affected, so they're tested together rather than in isolation:
 *
 *   1. hls::burst_maxi::read_request(addr, len) with len a genuine
 *      RUNTIME value (not a compile-time constant) -- this project has
 *      only ever synthesized burst_maxi with compile-time-constant
 *      lengths before (WRITEOUT round, len=1 always).
 *   2. A flat 1D row buffer (act_t row_buf[MAC_PR][MAX_CIN_TIMES_W]),
 *      deliberately NOT complete-partitioned on the large dimension,
 *      filled via runtime-indexed writes -- does HLS infer BRAM (cheap)
 *      or generate mux logic (expensive, the round-12/13 sparsemux
 *      failure class)?
 *   3. Copying a small MAC_PD x MAC_PR x MAC_PC slice out of that flat
 *      buffer, at a RUNTIME base offset, into a fully-partitioned
 *      destination matching pw_patch_full's real shape/access pattern --
 *      this is the step that would replace PW_STAGE's old per-(rt,colt,
 *      ot,cbase) restaging copy with a per-(rt,colt)-only copy (no ot/
 *      cbase redundancy) -- need its own real Iteration Latency/II, not
 *      an assumption that "no ot loop" automatically means cheap.
 *
 * MAX_CIN_TIMES_W=9216 is the REAL, verified joint bound (Cin x W) across
 * every real PWCONV layer in the 82-entry network (verified against
 * desc_all.bin directly, not estimated) -- NOT MAX_CIN x W_MAX (which
 * would be 1152 x 64 = 73,728, 8x larger, since Cin and W trade off
 * against each other in the real network: the largest Cin layers have
 * the smallest W and vice versa).
 *================================================================*/
#include "mac_array.h"
#include <cstdio>
#include <cstring>

#define MAX_CIN_TIMES_W 9216
#define MAX_WORDS (MAX_CIN_TIMES_W / 4)

void row_hoist_probe_top(
    hls::burst_maxi<ap_uint<32> > in_burst,
    int cin,            /* runtime, real range 1..1152 */
    int w_real,         /* runtime, real range 1..64 for real PW layers */
    int colt,           /* runtime, which MAC_PC-wide column slice to extract */
    act_t pw_patch_out[MAC_PD][MAC_PR][MAC_PC]
)
{
#pragma HLS INTERFACE m_axi port=in_burst offset=slave bundle=gmem_probe
#pragma HLS INTERFACE s_axilite port=cin bundle=control
#pragma HLS INTERFACE s_axilite port=w_real bundle=control
#pragma HLS INTERFACE s_axilite port=colt bundle=control
#pragma HLS INTERFACE s_axilite port=pw_patch_out bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    /* ZHR-92 (2026-08-25) round 2 correction: the real II=16 bottleneck,
     * per csynth's own "II Violation ... due to limited memory ports"
     * warnings, was pw_patch_out -- NOT row_buf as first (wrongly)
     * diagnosed. pw_patch_out had no ARRAY_PARTITION at all in round 1's
     * probe, an oversight -- the real mac_array.cpp's own analogous
     * destination buffer (lane_in[MAC_PD][MAC_PR][MAC_PC], same shape,
     * see run_reduce_unified) has always used a single
     * "complete dim=0" pragma. Matching that here. */
    #pragma HLS ARRAY_PARTITION variable=pw_patch_out complete dim=0

    /* Item 2: flat row buffer -- complete-partitioned on MAC_PR (dim1,
     * compile-time constant, cheap 4-way split) but NOT on the large
     * dimension. This is the whole question: does leaving it un-
     * partitioned let HLS map it to BRAM with plain runtime addressing
     * (the hoped-for outcome), or does something about how it's
     * accessed force per-element muxing anyway? */
    static act_t row_buf[MAC_PR][MAX_CIN_TIMES_W];
    #pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
    /* ZHR-92 (2026-08-25): cyclic factor=MAC_PC on the large dimension --
     * the COPY loop's 4-wide unrolled cw access reads 4 CONSECUTIVE
     * flat_idx values in the same cycle (colt*MAC_PC+cw for cw=0..3,
     * and colt*MAC_PC is always a multiple of MAC_PC=4) -- cyclic
     * factor=4 puts those 4 consecutive addresses in 4 DIFFERENT banks
     * (bank = flat_idx % 4), resolving the single-port contention that
     * measured as achieved II=16 with no partitioning on this dimension
     * at all (round 1 of this probe). This is a split, not a
     * duplication -- total storage is unchanged, each of the 4 banks is
     * 1/4 the depth. */
    #pragma HLS ARRAY_PARTITION variable=row_buf cyclic factor=MAC_PC dim=2

    int total_words = (cin * w_real + 3) / 4;   /* runtime value */

    /* Item 1: runtime-length read_request. Item 2: runtime-indexed fill
     * of the flat buffer. Compile-time-bounded trip count (MAX_WORDS),
     * runtime `valid` mask -- the established fix pattern for a
     * runtime-derived loop extent, not the loop's own bound directly. */
    ROW_READ: for (int rr = 0; rr < MAC_PR; rr++) {
        in_burst.read_request((size_t)rr * MAX_WORDS, (unsigned)total_words);
        FILL: for (int i = 0; i < MAX_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            bool valid = i < total_words;
            ap_uint<32> w = valid ? in_burst.read() : (ap_uint<32>)0;
            for (int b = 0; b < 4; b++) {
                #pragma HLS UNROLL
                int flat_idx = i * 4 + b;
                if (valid && flat_idx < MAX_CIN_TIMES_W) {
                    row_buf[rr][flat_idx] = (act_t)w.range(b * 8 + 7, b * 8);
                }
            }
        }
    }

    /* Item 3: copy MAC_PD x MAC_PR x MAC_PC slice out of row_buf at a
     * RUNTIME base offset (ci * w_real + colt*MAC_PC + cw -- the real
     * access pattern GATHER_ALL_PW/pw_flat_pipeline would need), into a
     * small fully-partitioned destination. Compile-time-bounded loops
     * throughout (MAC_PD/MAC_PR/MAC_PC are all compile-time constants in
     * the real design). This is the step standing in for "replace
     * PW_STAGE's per-(rt,colt,ot,cbase) copy with a per-(rt,colt)-only
     * copy" -- its own Iteration Latency/II is what determines whether
     * that 48x-fewer-copies claim actually holds in cycles, not just in
     * call count. */
    COPY: for (int dd = 0; dd < MAC_PD; dd++) {
        #pragma HLS PIPELINE II=1
        for (int rr = 0; rr < MAC_PR; rr++) {
            #pragma HLS UNROLL
            for (int cw = 0; cw < MAC_PC; cw++) {
                #pragma HLS UNROLL
                int flat_idx = dd * w_real + colt * MAC_PC + cw;
                bool valid = (dd < cin) && (flat_idx < MAX_CIN_TIMES_W);
                pw_patch_out[dd][rr][cw] = valid ? row_buf[rr][flat_idx] : (act_t)0;
            }
        }
    }
}
