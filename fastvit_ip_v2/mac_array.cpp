#include "mac_array.h"
/* ZHR-92 angle-B step (2026-08-24, final): explicit-API burst for
 * WRITEOUT (PW's own write, inside pw_flat_pipeline). Fast path
 * (hls::burst_maxi<ap_uint<32>>, out_burst) requires BOTH col_sz==MAC_PC
 * AND the row's byte address 4-aligned -- col_sz alone isn't sufficient,
 * confirmed by real csim failures (Phase4/10/14, all w_out=10) when only
 * col_sz was checked. Slow path (plain out_base store) otherwise. See
 * out_burst's own comment at mac_array_top's signature for why this
 * costs nothing on the real network (every real PW layer's w_out is a
 * multiple of MAC_PC or exactly 1, which already fails col_sz==MAC_PC
 * anyway) but is a real, resolution/MAC_PC-coupled constraint, not an
 * implementation footnote -- see mac_array.h's own note on this too. */
#include <hls_burst_maxi.h>
/* ZHR-92 angle-B step (2026-08-24): explicit-API burst probe for WRITEOUT
 * (PW's own write, inside pw_flat_pipeline). out_burst is
 * hls::burst_maxi<ap_uint<32>>, not <act_t> -- the previous attempt
 * (int8-typed burst_maxi on this same bundle) hit an internal LLVM-IR
 * codegen crash, plausibly from mixing an 8-bit view with the bundle's
 * real 32-bit AXI width (forced by in_base_wide already sharing
 * gmem_act). Matching the bundle's own width should avoid that mismatch.
 * See out_burst's own comment at mac_array_top's signature. */

/* Host-side only (see mac_array.h) -- NOT reachable from mac_array_top's
 * call graph, so none of this division is synthesized into hardware.
 * ch_dim is ALWAYS Cin (round 5): DW's real per-channel parallel tiling
 * (cin==cout for depthwise) and PW's Cin reduction-chunk stepping both
 * key off the same arithmetic. */
MacArrayParams derive_mac_array_params(const LayerDescV2 &d)
{
    MacArrayParams p;
    p.h_out = (d.h_in + 2 * d.pad - d.k) / d.stride + 1;
    p.w_out = (d.w_in + 2 * d.pad - d.k) / d.stride + 1;
    int ch_dim = d.cin;

    p.n_row_tiles = (p.h_out + MAC_PR - 1) / MAC_PR;
    p.n_col_tiles = (p.w_out + MAC_PC - 1) / MAC_PC;
    p.n_ch_tiles  = (ch_dim  + MAC_PD - 1) / MAC_PD;

    p.last_row_tile = p.h_out - (p.n_row_tiles - 1) * MAC_PR;
    p.last_col_tile = p.w_out - (p.n_col_tiles - 1) * MAC_PC;
    p.last_ch_tile  = ch_dim  - (p.n_ch_tiles  - 1) * MAC_PD;

    p.in_ch_stride  = d.h_in * d.w_in;
    p.out_ch_stride = p.h_out * p.w_out;
    return p;
}

static acc_t clip_shift(acc_t acc, int shift)
{
    acc_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return v;
}

/* ---- A3 round 3 (2026-08-21, ZHR-92): drive_mac is GONE, its body
 * folded directly into UNIFIED's loop. Round 14's drive_mac existed as a
 * SEPARATE function specifically so HLS's resource sharing across this
 * function's two mutually-exclusive (op_type-gated) call sites kept it
 * to one physical 512-wide instance -- confirmed working ever since (DSP
 * stayed ~110-118 across every round, never doubled to ~1024). But that
 * same function-call boundary is exactly what prevented DW_TAP_H/W and
 * UNIFIED_PW from ever carrying their own #pragma HLS PIPELINE --
 * putting PIPELINE on either CALLER loop was round 9/10's original
 * mistake (duplicated the shared instance, DSP 512->1024), since a
 * pipelined caller loop demands its own dedicated per-cycle-available
 * copy of whatever it calls. Measured cost of leaving it this way (A3
 * round 2, ZHR-92, board-measured): ~61 cycles per drive_mac invocation
 * for one pipeline step's worth of real 512-wide work -- call/return
 * handshake (`INLINE off`), lane_in/lane_w parameter marshalling, and
 * acc round-tripping through the function interface, paid EVERY step.
 *
 * Fix: checked first (not assumed) that drive_mac really did have two
 * textual call sites (grep confirmed: one in the DW branch, one in the
 * PW branch, both below) -- inlining drive_mac directly would remove the
 * function boundary the shared-instance property depends on, almost
 * certainly reintroducing round 9/10's DSP doubling. Instead: gather
 * EVERY step's lane_in/lane_w BEFORE the reduction, into
 * lane_in_all/lane_w_all (indexed by step -- cheap on-chip copying, not
 * unrolled, not the throughput-critical path), then run ONE single
 * pipelined loop (UNIFIED) that reads a step at a time and does the
 * 512-wide accumulate directly, inline, with no function call inside the
 * pipelined region at all. There is exactly ONE 512-wide unrolled
 * accumulate region in the whole design -- same "only one physical
 * instance" property round 14 achieved, just enforced by there being
 * only one copy of the code, not by a function boundary.
 *
 * Two pitfalls this deliberately avoids (both already paid for once, in
 * round 12/13's history):
 *   - op_type branching INSIDE the pipelined region: not done here --
 *     UNIFIED only ever indexes by `step`, a plain induction variable;
 *     op_type only selects which GATHER code fills lane_in_all/
 *     lane_w_all beforehand, outside the pipelined loop entirely.
 *   - runtime-derived indices (kh/kw from a flat step counter, dw_S-
 *     dependent strides) INSIDE the pipelined region: round 12's actual
 *     failure was `dw_patch[dd][rr*dw_S+kh][cw*dw_S+kw]` evaluated
 *     per-lane inside the 512-way unroll, which fanned into ~3000+
 *     sparsemux cores just to hold II=1. That same stride-dependent
 *     gather still exists here, but ONLY in the GATHER phase (ordinary
 *     loop, not unrolled, not throughput-critical) -- the exact same
 *     dw_S==1/dw_S==2 branch technique GATHER_DW_D_S1/S2 already used is
 *     reused verbatim so the multiply stays compile-time-resolvable.
 *     UNIFIED itself reads lane_in_all[step][dd][rr][cw] -- a clean,
 *     compile-time-shaped index into an already-gathered buffer, with
 *     nothing runtime-derived left to fan out. */
/* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2): renamed from
 * run_reduce_unified -- PW no longer calls this at all (see
 * pw_flat_pipeline, above run_layer), so op_type and every PW-only
 * parameter (pw_patch_full/pw_c0/pw_wtile) were dead weight left in a
 * runtime-int-gated branch HLS couldn't prove unreachable on its own,
 * same lesson already applied once before in the (reverted) pw_one_ot
 * round. Removed along with the branch; DW's own gather logic
 * (dw_S==1/dw_S==2, round 13's fix) and UNIFIED are unchanged. */
static void run_reduce_dw(
    int n_steps,
    const act_t dw_patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX],
    const wt_t  dw_wtile[MAC_PD][MAX_K][MAX_K],
    int dw_K, int dw_S,
    acc_t acc[MAC_PD][MAC_PR][MAC_PC])
{
    /* round 15: cyclic factor was hardcoded to 8 (matching the old fixed
     * MAC_PD=8) at all four sites in this file; caught while dropping
     * MAC_PD to 2 -- with a hardcoded 8, bank=(cib+dd) mod 8 stops being
     * compile-time-known for unrolled dd once cib=step*MAC_PD's stride no
     * longer equals the partition factor, reopening a runtime bank-select
     * cost round 5/8 already eliminated. Must always equal MAC_PD. */
    #pragma HLS ARRAY_PARTITION variable=dw_patch complete dim=0
    #pragma HLS ARRAY_PARTITION variable=dw_wtile complete dim=0
    #pragma HLS ARRAY_PARTITION variable=acc      complete dim=0

    /* Per-step gather buffers -- dim=1 (step) is deliberately NOT
     * partitioned/unrolled (UNIFIED below accesses one step at a time,
     * sequentially; only dims 2-4, the 512-wide lane shape, need to be
     * fully parallel-addressable). */
    act_t lane_in_all[MAX_STEPS][MAC_PD][MAC_PR][MAC_PC];
    wt_t  lane_w_all[MAX_STEPS][MAC_PD];
    #pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=2
    #pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=3
    #pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=4
    #pragma HLS ARRAY_PARTITION variable=lane_w_all  complete dim=2

    /* round 13's fix retained verbatim: DW gets its own doubly-nested
     * compile-time loop (kh, kw each 0..MAX_K-1, both literal bounds) so
     * kh/kw are genuine loop induction variables of directly-bounded
     * loops, not arithmetic derived from a shared flat counter. This
     * phase is NOT the pipelined/unrolled region (that's UNIFIED, below)
     * so a runtime-valued `step = kh*MAX_K+kw` write-index here is cheap
     * ordinary address-counter hardware, not the round-12 fan-out
     * problem (that was specifically about a runtime index feeding a
     * 512-way UNROLLED read). */
    if (dw_S == 1) {
        GATHER_ALL_DW_S1: for (int kh = 0; kh < MAX_K; kh++) {
            for (int kw = 0; kw < MAX_K; kw++) {
                int step = kh * MAX_K + kw;
                bool valid = (kh < dw_K) && (kw < dw_K);
                for (int dd = 0; dd < MAC_PD; dd++) {
                    lane_w_all[step][dd] = valid ? dw_wtile[dd][kh][kw] : (wt_t)0;
                    for (int rr = 0; rr < MAC_PR; rr++) {
                        for (int cw = 0; cw < MAC_PC; cw++) {
                            lane_in_all[step][dd][rr][cw] = dw_patch[dd][rr * 1 + kh][cw * 1 + kw];
                        }
                    }
                }
            }
        }
    } else {
        GATHER_ALL_DW_S2: for (int kh = 0; kh < MAX_K; kh++) {
            for (int kw = 0; kw < MAX_K; kw++) {
                int step = kh * MAX_K + kw;
                bool valid = (kh < dw_K) && (kw < dw_K);
                for (int dd = 0; dd < MAC_PD; dd++) {
                    lane_w_all[step][dd] = valid ? dw_wtile[dd][kh][kw] : (wt_t)0;
                    for (int rr = 0; rr < MAC_PR; rr++) {
                        for (int cw = 0; cw < MAC_PC; cw++) {
                            lane_in_all[step][dd][rr][cw] = dw_patch[dd][rr * 2 + kh][cw * 2 + kw];
                        }
                    }
                }
            }
        }
    }

    UNIFIED: for (int step = 0; step < n_steps; step++) {
        #pragma HLS PIPELINE II=1
        LANE_D: for (int dd = 0; dd < MAC_PD; dd++) {
            #pragma HLS UNROLL
            LANE_R: for (int rr = 0; rr < MAC_PR; rr++) {
                #pragma HLS UNROLL
                LANE_C: for (int cw = 0; cw < MAC_PC; cw++) {
                    #pragma HLS UNROLL
                    acc[dd][rr][cw] += (acc_t)lane_in_all[step][dd][rr][cw] * (acc_t)lane_w_all[step][dd];
                }
            }
        }
    }
}

/* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2): PW's flat
 * pipeline. Replaces PW's entire per-(rt,colt) (ot,cbase) loop +
 * WRITEOUT_PW with ONE counter-driven `PIPELINE II=1` loop, called ONCE
 * per (rt,colt) tile (handling every ot/cbase internally) instead of
 * the previous per-ot/per-cbase nested-loop round-trips. DW is
 * completely untouched -- still calls run_reduce_unified via its own
 * per-ot loop in run_layer below, unchanged, so stage 3's P&R cleanly
 * isolates PW's own resource/timing delta.
 *
 * See the design doc + stage-1 spike (flat_pipeline_probe.cpp) on
 * ZHR-92 for the full rationale and the four stage-1 findings carried
 * forward here:
 *   - II=2 achieved in the spike (not the targeted 1) -- acc's
 *     cross-iteration dependency stalls at the compute<->writeout phase
 *     transition. Still a real, large win if it survives here: the
 *     spike predicted ~64 cycles for an isolated n_cbase=1 ot (vs. the
 *     measured 870 today) and ~1,184 cycles for a 36-cbase real layer
 *     (vs. 11,852 today) -- roughly 10-13x fewer cycles.
 *   - Zero divide/modulo confirmed in the spike -- every derived index
 *     here is a loop-carried accumulator with wrap-pair logic, never a
 *     computed index/mod (round 13's `kh = step / MAX_K` mistake,
 *     avoided by construction).
 *   - LUT net delta is an accepted uncertainty, not resolved by the
 *     spike (which structurally couldn't see the "hidden run_layer
 *     glue" cost this whole investigation has been chasing) -- real P&R
 *     is the stop-loss (current baseline 96%, ~1,800 LUT headroom).
 *   - Burst inference is explicitly OUT OF SCOPE this round (10x
 *     flattening win prioritized over burst's 2-3x, already failed
 *     three times on the old WRITEOUT_PW shape) -- WRITEOUT keeps its
 *     boundary guard for partial tiles, same as before; revisit burst
 *     separately later once this shape has settled, re-diagnosing from
 *     scratch since the structure has changed.
 *
 * Generalized beyond the stage-1 spike to handle what real layers
 * actually need:
 *   - Partial last cbase chunk (Cin not a multiple of MAX_CIN_PW=32):
 *     each cbase is still a compile-time-fixed PW_FLAT_STEPS_PER_CBASE
 *     steps (not a runtime bound) -- the weight read is zero-filled for
 *     any channel >= Cin, same established pattern as PW_WSTAGE's
 *     existing zero-fill (nulls the product regardless of what
 *     pw_patch_full holds past Cin, same channel-bounds safety argument
 *     already used elsewhere in this file -- c0+cib+dd's max value is
 *     always < MAX_CIN, so the read itself is always safe even when the
 *     data is stale).
 *   - Partial spatial tile (r_sz<MAC_PR or col_sz<MAC_PC, e.g. layer
 *     50/51's w_out=1): WRITEOUT keeps its own boundary guard, unchanged
 *     correctness-wise from WRITEOUT_PW's guarded form.
 *   - use_shift_table: shift is read once per ot, at the compute->
 *     writeout phase transition (not every cycle) -- same
 *     loop-invariant-hoist discipline as the reverted WRITEOUT
 *     dual-path round's surviving half. */
#define PW_FLAT_STEPS_PER_CBASE (MAX_CIN_PW / MAC_PD)
#define PW_FLAT_WRITEOUT_ELEMS  (MAC_PR * MAC_PC)

/* ZHR-92 round (2026-08-27, PW_FLAT II=2->1): HLS's own diagnostic named
 * the exact cause -- two writes on the shared gmem_act-bundled port
 * (out_burst.write at the old line 367, out_base[]= at the old line 371)
 * in mutually-exclusive (if/else) branches, but the scheduler can't prove
 * across-iteration non-conflict, forcing II=2. FAST_WRITEOUT is now a
 * template parameter, not a runtime branch: each instantiation gets
 * dead-code-eliminated down to exactly ONE write mechanism, so there is
 * only ever one write site on the port per instantiation -- the conflict
 * the scheduler couldn't rule out is gone by construction, not asserted
 * away. Real network verified (layer_descriptor_256.json, all 52 layers):
 * every PW layer's w_out is a multiple of MAC_PC=4 except layers 50/51
 * (SE block fc1/fc2, h_in=w_in=1) -- so pw_flat_pipeline (FAST_WRITEOUT=
 * true) unconditionally assumes col_sz==MAC_PC and w_out%MAC_PC==0 (both
 * guaranteed by the dispatch in run_layer below, not re-checked here), and
 * pw_flat_pipeline_narrow (FAST_WRITEOUT=false) exists only to serve
 * those 2 layers via the original scalar out_base[] path, unchanged
 * behavior from before this round. All shared gather/compute/wrap-bound
 * logic (including the two previously-fixed bugs documented in this
 * function's own body) stays in ONE template body, not duplicated --
 * duplicating it was considered and rejected as the higher-risk path. */
template<bool FAST_WRITEOUT>
static void pw_flat_pipeline_impl(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    hls::burst_maxi<ap_uint<32> > &out_burst,
    int rt, int colt, int r_sz, int col_sz)
{
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic factor=MAC_PD dim=1
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3

    const int Cin = d.cin;
    const int n_ot = d.cout;
    const int n_cbase = (Cin + MAX_CIN_PW - 1) / MAX_CIN_PW;
    const int total_iters = n_ot * (n_cbase * PW_FLAT_STEPS_PER_CBASE + PW_FLAT_WRITEOUT_ELEMS);

    acc_t acc[MAC_PD][MAC_PR][MAC_PC];
    #pragma HLS ARRAY_PARTITION variable=acc complete dim=0
    /* ZHR-92 angle-B step (2026-08-24): one row (MAC_PC=4 act_t) packs
     * exactly into one 32-bit word -- accumulated a byte/cycle across the
     * row, flushed as a single-word burst_maxi write at the row's last
     * column (word address assumes byte address is 4-aligned; not
     * verified this round, same caveat as prior WRITEOUT probes). */
    ap_uint<32> row_word = 0;

    /* loop-carried state -- all plain counters, wrap via compare+add,
     * no derived multiply/divide/mod anywhere in the hot loop (see the
     * spike's own header comment for why this matters). */
    int k = 0;                  /* 0..PW_FLAT_STEPS_PER_CBASE-1, dual-use */
    bool in_writeout = false;
    int cbase_idx = 0;
    int ch_off = 0;              /* channel offset within Cin, shared by patch+weight addressing */
    int w_ot_base = 0;           /* == ot*Cin, accumulated */
    int ot_out_ch_base = 0;      /* == ot*d.out_ch_stride, accumulated */
    int ot_idx = 0;
    int wr_row = 0, wr_col = 0;  /* writeout row/col -- wrap-pair, not idx/MAC_PC and idx%MAC_PC */
    int shift_reg = d.out_shift; /* recomputed at each ot's compute->writeout transition */

    PW_FLAT: for (int i = 0; i < total_iters; i++) {
        #pragma HLS PIPELINE II=1
        bool reset_acc = (!in_writeout) && (cbase_idx == 0) && (k == 0);

        if (!in_writeout) {
            wt_t lane_w[MAC_PD];
            act_t lane_in[MAC_PD][MAC_PR][MAC_PC];
            #pragma HLS ARRAY_PARTITION variable=lane_w complete dim=0
            #pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                bool ch_valid = (ch_off + dd) < Cin;
                lane_w[dd] = ch_valid ? w_base[d.w_off + w_ot_base + ch_off + dd] : (wt_t)0;
                for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        lane_in[dd][rr][cw] = pw_patch_full[ch_off + dd][rr][cw];
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
            /* ZHR-92 angle-B step (2026-08-24, final -- dual path): a
             * single 32-bit burst_maxi write is atomic, so the fast path
             * needs BOTH the row full (col_sz==MAC_PC) AND the row's own
             * byte address 4-aligned -- col_sz alone is NOT sufficient
             * (csim-confirmed: Phase4/10/14, all w_out=10, corrupted data
             * even on col_sz==MAC_PC rows, because (rt*MAC_PR+wr_row)*
             * d.w_out isn't guaranteed 4-aligned unless d.w_out itself
             * is). Real network cost: zero -- every real PW layer's
             * w_out is a multiple of MAC_PC or exactly 1 (entry76/78,
             * SE block fc1/fc2), and w_out=1 already fails col_sz==MAC_PC
             * on its own, so the alignment check never changes the
             * real-network verdict, only the synthetic test shapes'.
             * This IS a real, standing constraint coupled to resolution/
             * MAC_PC choice, not an implementation footnote -- see
             * out_burst's header comment and mac_array.h's own note. */
            act_t val = 0;
            if (wr_row < r_sz && wr_col < col_sz) {
                acc_t total = 0;
                for (int dd = 0; dd < MAC_PD; dd++) {
                    #pragma HLS UNROLL
                    total += acc[dd][wr_row][wr_col];
                }
                total += pw_bias_cache[ot_idx];
                val = (act_t)clip_shift(total, shift_reg);
            }
            /* A3 shared-multiplier round (2026-08-25, ZHR-92, U2598
             * candidate #1) -- ATTEMPTED AND REVERTED. Converting
             * (rt*MAC_PR+wr_row)*d.w_out below to an rt-loop accumulator
             * (run_layer) + a 4-entry wr_row table (built via 3 adds, same
             * call shape as this file's other accumulator conversions) held
             * csim 17/17 but did NOT change mul_32s_32s_32_2_1's real
             * instance count in the exported RTL (still 2, same as
             * baseline) -- this term is not one of the two surviving
             * physical instances. Ruled out, not a dead end: the real
             * owner of those two instances is still unidentified. */
            if (wr_row < r_sz) {
                int byte_addr = d.out_off + ot_out_ch_base + (rt * MAC_PR + wr_row) * d.w_out + colt * MAC_PC;
                if (FAST_WRITEOUT) {
                    row_word.range(wr_col * 8 + 7, wr_col * 8) = val;
                    if (wr_col == MAC_PC - 1) {
                        out_burst.write_request(byte_addr >> 2, 1);
                        out_burst.write(row_word);
                        out_burst.write_response();
                    }
                } else {
                    if (wr_col < col_sz) {
                        out_base[byte_addr + wr_col] = val;
                    }
                }
            }
        }

        /* A3 shared-multiplier round (2026-08-25, ZHR-92, MAC_PD=1 sweep):
         * FOUND AND FIXED via csim, not assumed correct -- k's wrap bound
         * used to be PW_FLAT_STEPS_PER_CBASE unconditionally, for BOTH the
         * gather phase (where that's the right bound) AND the writeout
         * phase (which actually needs PW_FLAT_WRITEOUT_ELEMS=MAC_PR*MAC_PC
         * steps, a spatial-tile constant, unrelated to Cin-chunking). At
         * MAC_PD=2 these two bounds happened to both equal 16
         * (MAX_CIN_PW/MAC_PD == MAC_PR*MAC_PC) -- a numerical coincidence
         * that masked this bug for this whole project's history. At
         * MAC_PD=1 the gather bound becomes 32, so writeout (still using
         * the same k/bound) ran 32 steps instead of 16 -- total_iters
         * (computed assuming writeout=PW_FLAT_WRITEOUT_ELEMS) then
         * underestimated the real step count, terminating PW_FLAT early
         * and dropping the last few output channels entirely (verified:
         * Phase1's cin=20/cout=13 shape lost exactly 3/13 channels in an
         * isolated Python state-machine simulation before this fix, 13/13
         * correct after). Phase-dependent wrap bound restores both. */
        int wrap_bound = in_writeout ? PW_FLAT_WRITEOUT_ELEMS : PW_FLAT_STEPS_PER_CBASE;
        if (k == wrap_bound - 1) {
            k = 0;
            if (!in_writeout) {
                if (cbase_idx == n_cbase - 1) {
                    in_writeout = true;
                    shift_reg = d.use_shift_table ? (int)pw_shift_cache[ot_idx] : d.out_shift;
                } else {
                    /* A3 round (2026-08-23, ZHR-92, run_layer rewrite
                     * stage 2 followup): FOUND AND FIXED via csim, not
                     * assumed correct -- ch_off is already sitting at
                     * cbase_idx*MAX_CIN_PW + (PW_FLAT_STEPS_PER_CBASE-1)*
                     * MAC_PD at this point (advanced by MAC_PD every
                     * non-wrap step above), which equals
                     * (cbase_idx+1)*MAX_CIN_PW - MAC_PD exactly, since
                     * PW_FLAT_STEPS_PER_CBASE*MAC_PD == MAX_CIN_PW by
                     * construction. One more +=MAC_PD (the SAME increment
                     * every other step uses) lands exactly on the next
                     * cbase's base -- += MAX_CIN_PW here overshot by a
                     * whole chunk (caught by csim: Phase12/13's real
                     * n_cbase>1 shapes failed, 8-15% mismatches, while
                     * every n_cbase==1 phase stayed clean). */
                    cbase_idx++;
                    ch_off += MAC_PD;
                }
            } else {
                in_writeout = false;
                cbase_idx = 0;
                ch_off = 0;
                w_ot_base += Cin;
                ot_out_ch_base += d.out_ch_stride;
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

static void pw_flat_pipeline(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    hls::burst_maxi<ap_uint<32> > &out_burst,
    int rt, int colt, int r_sz, int col_sz)
{
    pw_flat_pipeline_impl<true>(d, w_base, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt, colt, r_sz, col_sz);
}

/* Serves only layers whose w_out isn't a multiple of MAC_PC -- verified
 * against layer_descriptor_256.json this round: layers 50/51 (SE block
 * fc1/fc2, h_in=w_in=1) are the only two in the real network. Dispatched
 * from run_layer below via n_col_tiles==1 && last_col_tile<MAC_PC, the
 * same condition that makes col_sz<MAC_PC true for every tile of these
 * layers (there is only one tile). */
static void pw_flat_pipeline_narrow(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    hls::burst_maxi<ap_uint<32> > &out_burst,
    int rt, int colt, int r_sz, int col_sz)
{
    pw_flat_pipeline_impl<false>(d, w_base, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt, colt, r_sz, col_sz);
}

/* ---- round 11: run_dwconv/run_pwconv are GONE. This is the only tile
 * driver in the design, and mac_array_top (below) calls it unconditionally
 * -- no more `if (op==DW) run_dwconv() else run_pwconv()`. That dispatch
 * was round 9/10's actual root cause (see run_reduce_unified's header):
 * as long as DW and PW were two separate functions, each got its own
 * private call site into whatever shared step function existed, no matter
 * what that function was named or how its own PIPELINE was structured.
 *
 * DW and PW's outer tiling genuinely differs in what varies per "output
 * tile" (`ot`): DW's `ot` is a channel-tile (8 channels, needs its own
 * patch+weight staging every ot); PW's `ot` is a single output channel
 * processed sequentially against spatial input staged ONCE per (rt,colt)
 * and reused across every ot. Rather than force identical staging cost
 * models onto both (not needed for this round's question -- DSP, not
 * staging efficiency), both keep their own STAGE/WSTAGE and WRITEOUT
 * bodies, branched on op_type. What's unconditional and genuinely shared:
 * the (rt,colt,ot) loop nest itself (one nest, reused by both), RESET,
 * and the single call into run_reduce_unified. */
static void run_layer(const LayerDescV2 &d,
                       const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                       act_t out_base[], const ap_uint<32> in_base_wide[],
                       hls::burst_maxi<ap_uint<32> > &out_burst,
                       hls::burst_maxi<ap_uint<32> > &in_burst)
{
    const int Hin = d.h_in, Win = d.w_in;
    const int K = d.k, S = d.stride, P = d.pad;
    const int patch_r = (MAC_PR - 1) * S + K;
    const int patch_c = (MAC_PC - 1) * S + K;
    const int Cin = d.cin, H = d.h_in, W = d.w_in;

    const int n_ot = (d.op_type == LDESC_OP_DWCONV) ? d.n_ch_tiles : d.cout;

    /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2): the old
     * `n_cbase` local (PW's Cin-chunking count) is GONE -- pw_flat_pipeline
     * computes its own copy internally now, chunking is entirely PW's own
     * concern, not shared with run_layer's DW-only code below. */

    /* A3 round (2026-08-21, ZHR-92): weight hoist -- ATTEMPTED AND
     * REVERTED (2026-08-22, same round as the DW loop-bound fix above).
     * pw_weight_cache (442,368 elements = ~432KB) was the #2 worst P&R
     * timing path (PW_WEIGHT_HOIST -> pw_weight_cache BRAM write, 95%
     * route-delay-dominated) and a major share of BRAM's 89-95%
     * congestion, for a measured gain of only 1.3% (700.27ms -> 690.37ms)
     * -- a bad trade once the design is timing-negative. Reverted to
     * direct per-(ot,cbase) DRAM reads in PW_WSTAGE below (the pre-hoist
     * form, restored verbatim from that read site's own preserved
     * comment). pw_bias_cache is untouched: at 1152 elements (~4.6KB)
     * it's not implicated in either worst path and wasn't part of this
     * round's diagnosis -- reverting it would be an unrequested change. */
    static acc_t pw_bias_cache[MAX_PW_BIAS_CACHE];
    if (d.op_type == LDESC_OP_PWCONV) {
        PW_BIAS_HOIST: for (int oc = 0; oc < d.cout; oc++) {
            #pragma HLS PIPELINE II=1
            pw_bias_cache[oc] = b_base[d.b_off + oc];
        }
    }

    /* ZHR-92 round (2026-08-27, PW_FLAT II=2->1, second cause): same
     * hoist pattern as pw_bias_cache directly above. HLS's own diagnostic
     * (gmem_w port contention) named the exact conflict: on the specific
     * PW_FLAT iteration where an ot's last cbase's last gather step also
     * triggers the compute->writeout transition, the regular per-iteration
     * gather weight read (w_base[...]) and the shift-table read
     * (w_base[d.shift_off+ot_idx], only when d.use_shift_table) land on
     * the SAME iteration -- two genuine same-cycle reads on gmem_w, not a
     * cross-iteration scheduling artifact like the gmem_act case. Hoisting
     * the whole per-ot shift table into an on-chip cache before PW_FLAT
     * starts (unconditionally populated; harmless/unused when
     * use_shift_table=0) removes the read from the hot loop entirely. */
    static wt_t pw_shift_cache[MAX_PW_BIAS_CACHE];
    if (d.op_type == LDESC_OP_PWCONV && d.use_shift_table) {
        PW_SHIFT_HOIST: for (int oc = 0; oc < d.cout; oc++) {
            #pragma HLS PIPELINE II=1
            pw_shift_cache[oc] = w_base[d.shift_off + oc];
        }
    }

    for (int rt = 0; rt < d.n_row_tiles; rt++) {
        int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;

        /* A3 row-hoist round (2026-08-25, ZHR-92) stage 1: PW-only row-
         * level burst hoist. PW_PATCH_HOIST (below, per-(rt,colt)) issues
         * Cin*MAC_PR non-burst word reads EVERY colt tile, re-reading the
         * SAME row data 16x/row (measured: 192 transactions/tile, ~86
         * cycles each, entry3). Fix: read each (ci,rr) row ONCE per rt (not
         * per (rt,colt)) via a runtime-length hls::burst_maxi burst
         * covering the full row (w_in bytes, not just MAC_PC=4), into
         * row_buf; every colt then does a cheap SRAM-only COPY out of it.
         * Combined Phase-1 probe (row_hoist_probe/, isolated, not part of
         * this file) validated the two riskiest primitives before this
         * code was written: (1) burst_maxi::read_request with a genuine
         * RUNTIME length synthesizes clean (ManualBurstInstancePassed,
         * Length="variable") -- this project's first runtime-length burst;
         * (2) a flat runtime-indexed row buffer, correctly partitioned,
         * reaches II=1 on both the fill and the copy-out side. Partition
         * scheme (complete dim=1 / cyclic factor=MAC_PC dim=2) is the
         * exact scheme the probe's second round confirmed reaches II=1 --
         * the probe's FIRST round (row_buf partitioned, destination array
         * NOT) stuck at II=16; the real bottleneck was the destination
         * array's own missing partition, not row_buf, a correction to my
         * own initial (wrong) diagnosis. pw_patch_full below already has
         * the equivalent destination-side partitioning this needs (cyclic
         * factor=MAC_PD dim=1, complete dim=2/3) -- the real COPY loop's
         * ci is the loop's own sequential induction variable, not unrolled
         * (unlike the probe's dd), so it needs LESS destination
         * parallelism than the probe tested, not more; no new pragma
         * needed on pw_patch_full. MAX_CIN_TIMES_W=9216 (see mac_array.h)
         * is the verified real product bound Cin*w_in, not MAX_CIN*W_MAX. */
        static act_t row_buf[MAC_PR][MAX_CIN_TIMES_W];
        #pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
        #pragma HLS ARRAY_PARTITION variable=row_buf cyclic factor=MAC_PC dim=2

        /* A3 row-hoist round (2026-08-25, ZHR-92, slow-path removal): the
         * W%4==0 gate this comment used to describe is GONE -- real DRAM
         * layout is channel-major (in_ch_stride=H*W, confirmed via
         * derive_mac_array_params), so a row's Cin channels are NOT
         * contiguous in DRAM, each (ci,rr) its own burst of W bytes, but
         * ROW_READ_FILL below (word_addr0=byte_addr>>2, r=byte_addr&3,
         * n_words=(r+W+3)>>2) is a fully general byte-run extraction with
         * no dependency on W%4 or on r==0 -- verified by hand-derivation
         * for W=1 (entry76/78, the only real layers the old gate excluded):
         * n_words reduces to exactly 1 for every alignment residue r, and
         * the single valid byte lane is extracted correctly regardless of
         * r. Unconditional for every PWCONV layer now -- the old
         * PW_PATCH_HOIST slow path this gate used to fall back to is
         * deleted (see COPY_FROM_ROW's own header comment below). */
        /* A3 row-hoist round (2026-08-25, ZHR-92) shared-multiplier followup
         * -- ATTEMPTED AND REVERTED, kept as a TODO for the eventual 150MHz
         * multiplier cleanup (Phase D), not a dead end. Replacing this
         * loop's `oh*W` (oh=rt*MAC_PR+rr) with an accumulator pair
         * (row_rt_off accumulated across the outer rt loop via
         * MAC_PR*W-a-shift, rr_off accumulated across this rr loop via
         * 3 adds: 0,W,2W,3W) DID genuinely eliminate mul_32s_32s_32_2_1
         * from run_layer's own csynth report entirely (0 occurrences,
         * confirmed, not the pre-existing ci*in_ch_stride site which was
         * already accumulator-based before this round) -- csim stayed
         * 17/17. But real P&R showed this was the WRONG target: the
         * critical path's source/destination (ap_CS_fsm_reg[68] ->
         * mul_32s_32s_32_2_1_U2598) was BYTE-FOR-BYTE IDENTICAL before and
         * after -- same FSM register, same DSP instance suffix, DSP count
         * unchanged 66->66 -- proving that specific instance belongs to
         * pw_flat_pipeline's own untouched WRITEOUT term
         * ((rt*MAC_PR+wr_row)*d.w_out), not to this loop. Net P&R effect
         * was a real regression (WNS -0.181973ns -> -0.347903ns, -0.166ns)
         * for only -90 LUT -- not worth it, reverted. Diagnostic lesson:
         * "this round added a new multiply, therefore it's the newly-
         * surfaced critical path's source" was an unverified assumption --
         * the path already existed across three PRIOR rounds per CLAUDE.md
         * (logic levels 4->4->5), and _U2598's literal suffix survived
         * this whole round unchanged, which is what actually proved the
         * misattribution. Before touching this again: read _U2598's real
         * source line directly from csynth's own multiplier-instance list
         * (pw_flat_pipeline_csynth.rpt), don't re-infer it. */
        bool row_hoist_ok = (d.op_type == LDESC_OP_PWCONV);
        if (row_hoist_ok) {
            ROW_READ: for (int rr = 0; rr < MAC_PR; rr++) {
                /* Row-validity zero-fill, not a guard: ALWAYS issue the
                 * read (oh clamped to a safe in-bounds row, same r_valid?
                 * rr:0 clamp PW_PATCH_HOIST's slow path already uses) --
                 * never skip the read_request call itself. This is the
                 * READ side of the read/store asymmetry CLAUDE.md's
                 * WRITEOUT finding already established (a store can't be
                 * unconditional the way a read can); COPY_FROM_ROW below
                 * is what actually discards this row's data downstream via
                 * a data-path valid, not this loop. */
                bool r_valid = rr < r_sz;
                int oh = rt * MAC_PR + (r_valid ? rr : 0);
                int ch_base = 0;    /* == ci * d.in_ch_stride, accumulated -- no runtime multiply */
                int flat_base = 0;  /* == ci * W, accumulated -- row_buf's own flat layout */
                ROW_READ_CH: for (int ci = 0; ci < Cin; ci++) {
                    int byte_addr = d.in_off + ch_base + oh * W;
                    int word_addr0 = byte_addr >> 2;
                    int r = byte_addr & 3;
                    int n_words = (r + W + 3) >> 2;
                    in_burst.read_request((size_t)word_addr0, (unsigned)n_words);
                    ROW_READ_FILL: for (int i = 0; i < MAX_WORDS_PER_CH; i++) {
                        #pragma HLS PIPELINE II=1
                        bool word_valid = i < n_words;
                        ap_uint<32> wd = word_valid ? in_burst.read() : (ap_uint<32>)0;
                        for (int b = 0; b < 4; b++) {
                            #pragma HLS UNROLL
                            int pos = i * 4 + b - r;
                            bool valid = word_valid && (pos >= 0) && (pos < W);
                            if (valid) {
                                row_buf[rr][flat_base + pos] = (act_t)wd.range(b * 8 + 7, b * 8);
                            }
                        }
                    }
                    ch_base += d.in_ch_stride;
                    flat_base += W;
                }
            }
        }

        for (int colt = 0; colt < d.n_col_tiles; colt++) {
            int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

            /* A3 round 2 (2026-08-21, ZHR-92): loop-invariant hoist, same
             * pattern as the weight+bias hoist above -- PW's spatial patch
             * depends only on (rt,colt), not ot, but PW_STAGE used to
             * restage it from DRAM once per (rt,colt,ot,cbase). Stage the
             * full-Cin patch for THIS (rt,colt) tile once, here, reused by
             * every ot/cbase below. No boundary check needed (PW is always
             * k=1/stride=1/pad=0, no receptive-field overhang past r_sz/
             * col_sz -- same as the original PW_STAGE). Dummy/unused for
             * DW (guarded by the op_type check, same convention as the
             * weight hoist). */
            static act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC];
            /* A3 round (2026-08-23, ZHR-92): full partition, matching what
             * the chunk-local pw_patch used to have at 1/36th the depth --
             * same cyclic-by-MAC_PD / complete-dims-2,3 shape, just applied
             * to the real MAX_CIN=1152-deep storage instead of a 32-deep
             * copy of it. This is what actually eliminates PW_STAGE's copy
             * below: GATHER_ALL_PW (in run_reduce_unified) now reads this
             * array directly at a per-lane-parallel-addressable offset,
             * the same property the copy used to exist purely to provide. */
            #pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic factor=MAC_PD dim=1
            #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
            #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3
            /* A3 round 5 (2026-08-22, ZHR-92) -- ATTEMPTED AND REVERTED,
             * kept here as a TODO, not a dead end. PW_STAGE (the copy from
             * pw_patch_full into the smaller per-cbase pw_patch, below) is
             * a real, correctly-diagnosed cost: ~71% of the whole 179ms
             * (127ms), confirmed via the csynth report's own module
             * latency breakdown, not inferred. The fix (have
             * run_reduce_unified read pw_patch_full directly, fully
             * parallel-partitioned, eliminating PW_STAGE) is also
             * correct -- csim 15/15, GATHER_ALL_PW held II=1. What killed
             * it was resource headroom, not the approach: a pragma-only
             * probe undersold the real cost (+2.7% LUT/+16 BRAM_18K
             * estimated vs +32 BRAM_18K/real-BRAM-95%/WNS -1.289ns-even-
             * after-phys_opt_design actually measured) -- the probe never
             * went through real P&R, and at 90%+ utilization this
             * project's own "real P&R beats the csynth estimate by ~25
             * points" experience stopped holding. Reverted to this
             * (PW_STAGE-intact, 179.38ms, board-verified byte-exact)
             * state as the known-good baseline for the end-to-end round.
             * Three ready-to-try restart paths once resource margin exists
             * again, none attempted yet: (1) drop array width 32->16 to
             * free enough BRAM/LUT for the full parallel partition; (2) a
             * descriptor flag letting the generator choose PW_STAGE vs.
             * direct-read per layer, keeping the fix for small/medium
             * layers where it already fits and falling back for large
             * ones; (3) pair this with option 2's ot-tiled accumulator
             * (also costed, not implemented) to cap worst-case storage
             * instead of needing pw_patch_full's full width live at once.
             *
             * RETRIED this round (2026-08-23, ZHR-92): resource conditions
             * are now BRAM 9.3%/LUT 58.85%/WNS +0.115ns (vs. 95%/95%/
             * -1.289ns above) after the pw_weight_cache revert and the
             * gmem_act_wide merge freed real headroom -- the +32 BRAM_18K
             * this attempt actually cost last time is ~32% of today's
             * budget, not ~100%+. None of the three restart paths above
             * were needed; retrying the original direct approach as-is.
             * Per the lesson already recorded two paragraphs up, the
             * csynth numbers below are a magnitude check only -- the
             * actual go/no-go judgment is real P&R (see round writeup on
             * ZHR-92 for the P&R result this comment predates). */
            if (d.op_type == LDESC_OP_PWCONV) {
                /* A3 round (2026-08-22, ZHR-92, code-review followup):
                 * rr/cw used to be bounded by the runtime r_sz/col_sz --
                 * same class of hazard as the DW fix above (a loop trip
                 * count, not just a data value, depending on a runtime
                 * comparison), just never the one that happened to blow up:
                 * pw_patch_full's dims 2/3 aren't ARRAY_PARTITIONed (plain
                 * BRAM, not per-element registers), so this specific site
                 * was never actually at risk -- but bounding it at the
                 * compile-time MAC_PR/MAC_PC with a data-path `valid` closes
                 * the pattern structurally instead of relying on that being
                 * true forever. Address clamped to a known-in-range
                 * constant (0) when invalid, matching the DW fix's
                 * discipline. */
                /* A3 round (2026-08-23, ZHR-92, MERGE): the wide/narrow
                 * branch split (see git history for the pre-merge form) is
                 * GONE -- both cases now go through ONE word read via
                 * in_base_wide, unconditionally, regardless of
                 * d.use_wide_path (that field is no longer read here; see
                 * its header comment). Motivation, from the P&R round that
                 * added the 5th master (solution18, WNS -0.292ns): the
                 * critical path was NOT gmem_meta-related (Option E's old
                 * "zero logic levels, 74% routing delay" signature) but
                 * entirely internal to run_layer -- an FSM state bit
                 * driving a 32x32 multiplier's DSP cascade register (4
                 * logic levels, 58% logic delay). Prime suspect: the wide
                 * and narrow branches each computed their own
                 * `ci * d.in_ch_stride` product, and being mutually
                 * exclusive (only one runs per call), HLS's resource
                 * binding pass plausibly shared ONE physical multiplier
                 * between them, gated by an FSM-state mux feeding its
                 * operand register -- matching the observed path exactly.
                 * Supporting (not conclusive) evidence: DSP only rose
                 * 63->73 (+10) adding the wide path, not the ~35-70 a
                 * second independent 32x32 multiply chain would cost if
                 * unshared. This merge removes the second branch's
                 * multiply entirely (one `ci * d.in_ch_stride` site, not
                 * two) AND removes the separate gmem_act_wide master
                 * (folded back onto bundle=gmem_act, below) -- both changes
                 * follow from the same root decision (one unified read
                 * path), tested together as this round's one variable; the
                 * causal claim that this fixes the FSM->DSP path is NOT
                 * verified yet, only P&R can confirm it (see round report).
                 * Byte addressing is a general mod-4 decomposition of the
                 * SAME address the old narrow path used (d.in_off +
                 * ci*d.in_ch_stride + oh*W + colt*MAC_PC + cw), not an
                 * alignment assumption -- see the followup comment below
                 * for why a naive single-word-read version of this wasn't
                 * actually correct in general, and the fix. Csim coverage:
                 * Phase6 (desc6[1]/desc6[3], cin=10/4, h_in=w_in=1,
                 * in_off=360/374 -- 374 is NOT 4-aligned) covers the
                 * layer-50/51 narrow shape; Phase4 (desc4[1], cin=32,
                 * w_out=10, genuine partial last column tile col_sz=2)
                 * covers the wide-with-partial-tile case. No new phase
                 * needed -- both were pre-existing tests. */
                /* A3 round (2026-08-23, ZHR-92, MERGE followup): FOUND AND
                 * FIXED via csim, not assumed correct -- the first version
                 * of this loop (one word read per (ci,rr), lane=base_lane+cw)
                 * crashed csim ("Hi(39) out of bound(32) in range()") on
                 * Phase4's desc4[1] (PW, cin=32, w_out=10 -> a genuine
                 * PARTIAL last column tile, col_sz=2, at a non-4-aligned
                 * offset -- a pre-existing round-9 synthetic shape, never
                 * designed with word-alignment in mind). The "wide layers
                 * are always 4-aligned" invariant only holds for the real
                 * 82-entry network (verified there), not for arbitrary
                 * csim shapes -- and csim has to pass on ALL of them, not
                 * just the production-relevant ones. Fix: general 2-word
                 * read, second word fetched ONLY when a valid cw's byte
                 * would actually fall in it (need_word1) -- for the
                 * aligned/full-tile case (every real wide-eligible layer)
                 * need_word1 is always false, so this costs zero extra
                 * reads versus the original single-word design; the extra
                 * read only happens for misaligned/partial tiles like
                 * Phase4's, which is correctness insurance, not something
                 * the real network pays for. */
                /* A3 round (2026-08-23, ZHR-92, accumulator rewrite): three
                 * independent P&R rounds (wide-port, merge, PW_STAGE
                 * elimination) all found the SAME critical path -- an FSM
                 * state bit driving mul_32s_32s_32_2_1's DSP cascade
                 * register, logic levels climbing 4->4->5 each round even
                 * though only the LAST round touched DW's own code. Root
                 * cause: every `index * stride`-shaped address term in
                 * run_layer (this one, DW_PATCH_STAGE's, DW_WT_STAGE's,
                 * WRITEOUT_DW's, WRITEOUT_PW's) is getting bound to ONE
                 * shared physical multiplier, selected by FSM state --
                 * each additional call site deepens that selection tree by
                 * one level. Fix, applied at all five sites together (same
                 * mechanism, same round, same attribution as the
                 * DW_WT_STAGE+DW_PATCH_STAGE precedent): replace
                 * `ci * d.in_ch_stride` with a loop-carried accumulator
                 * (in_ch_base, incremented by d.in_ch_stride once per ci
                 * step) -- an ADD, not a multiply, so there is no multiply
                 * left here to bind. NOT the same shape as round 2's
                 * rejected accumulator attempt (that one was INSIDE the
                 * 512-wide UNIFIED unrolled region, needing one accumulator
                 * PER LANE, +5 DSP/+247 LUT for the privilege); this is an
                 * ordinary sequential staging loop, one accumulator total. */
                /* A3 row-hoist round (2026-08-25, ZHR-92, slow-path removal):
                 * the W%4==0-gated dual path (see row_hoist_ok's own header
                 * comment above) is GONE -- hand-derivation (not tested,
                 * checked by inspection before this change) showed
                 * ROW_READ's general byte-run extraction (n_words=
                 * (r+W+3)>>2, pos=i*4+b-r masking) was never actually
                 * specialized to W%4==0 in the first place; that gate was a
                 * conservative carryover from the original dual-path plan
                 * (which assumed WRITEOUT's fast/slow split as the template),
                 * not a real requirement of the code as written. Verified
                 * for W=1 specifically (entry76/78, the only real layers
                 * this excluded): n_words reduces to exactly 1 for every
                 * alignment residue r, and the single valid byte lane
                 * (pos==0 iff b==r) is extracted correctly regardless of r.
                 * The old PW_PATCH_HOIST block (per-(rt,colt) direct DRAM
                 * read via in_base_wide) is now dead code -- grep-confirmed
                 * before deleting, not assumed: PW_PATCH_HOIST served ONLY
                 * PWCONV (DW has its own separate DW_PATCH_STAGE/
                 * DW_WT_STAGE, untouched), and in_base_wide had no other
                 * reader anywhere in this file. in_base_wide's m_axi
                 * parameter/interface itself is left declared (unused, not
                 * removed) -- same "kept declared, harmless, avoids a
                 * driver/testbench/header ripple" convention already
                 * established for use_wide_path's own dead-field history. */
                int flat_base = 0;   // == ci * W, accumulated
                COPY_FROM_ROW: for (int ci = 0; ci < Cin; ci++) {
                    #pragma HLS PIPELINE II=1
                    for (int rr = 0; rr < MAC_PR; rr++) {
                        #pragma HLS UNROLL
                        for (int cw = 0; cw < MAC_PC; cw++) {
                            #pragma HLS UNROLL
                            bool valid = (rr < r_sz) && (cw < col_sz);
                            pw_patch_full[ci][rr][cw] = valid
                                ? row_buf[rr][flat_base + colt * MAC_PC + cw]
                                : (act_t)0;
                        }
                    }
                    flat_base += W;
                }
            }

            /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2):
             * PW no longer goes through this per-ot loop at all -- see
             * pw_flat_pipeline's own header comment above run_layer.
             * The old shared `ot_out_ch_base` accumulator (PW-only, per
             * its own removed comment) went with it; DW's own per-channel
             * addressing (oc_ch_tbl, below) never used it. */
            if (d.op_type == LDESC_OP_DWCONV) {
            for (int ot = 0; ot < n_ot; ot++) {
                /* known simplification, not yet addressed: DW's receptive-
                 * field reads overlap between spatial lanes (sliding
                 * window), so unlike PW's disjoint Cin banking there is no
                 * clean compile-time-constant bank assignment here without
                 * a real line-buffer/shift-register redesign -- kept
                 * `complete` (register-file, correct but not necessarily
                 * cheap). */
                act_t dw_patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX];
                wt_t  dw_wtile[MAC_PD][MAX_K][MAX_K];
                acc_t dw_btile[MAC_PD];
                #pragma HLS ARRAY_PARTITION variable=dw_patch complete dim=0
                #pragma HLS ARRAY_PARTITION variable=dw_wtile complete dim=0
                #pragma HLS ARRAY_PARTITION variable=dw_btile complete dim=0

                int c_sz = MAC_PD;
                int n_f = 1;

                /* A2 pre-step (2026-08-21): fpg (filters-per-group) was
                 * carried in LayerDescV2 but never read anywhere in this
                 * file -- DW implicitly assumed fpg=1 (cout==cin, one
                 * filter per input channel). Real network has 4 layers
                 * (3 stage-downsamples + final_conv) at fpg=2: each INPUT
                 * channel produces fpg INDEPENDENT output channels, each
                 * with its own K*K filter, but still only reducing over
                 * that SAME single input channel's receptive field (no
                 * cross-channel accumulation -- still depthwise-shaped,
                 * just multiple filters per input channel instead of one).
                 * Same failure class as the K=7 gap: silently wrong on
                 * those 4 layers today, no crash. Fix: patch staging
                 * (reads the input, doesn't depend on which output filter)
                 * stays a single per-ot step; weight/bias staging, RESET,
                 * the MAC reduction, and WRITEOUT now repeat for f=0..fpg-1,
                 * each producing output channel oc = c*fpg+f from the SAME
                 * staged patch with a DIFFERENT weight set. n_f=1 for PW
                 * (fpg always 1 there) makes this loop a no-op wrapper for
                 * the PW path -- unchanged behavior. */
                if (d.op_type == LDESC_OP_DWCONV) {
                    c_sz = (ot == d.n_ch_tiles - 1) ? d.last_ch_tile : MAC_PD;
                    n_f = d.fpg;
                    /* A3 round (2026-08-22, ZHR-92): c_sz is a runtime value
                     * (derived from d.n_ch_tiles/d.last_ch_tile, both read
                     * off the gmem_meta AXI path -- P&R's own critical path,
                     * WNS=+0.112ns, zero logic levels/74% routing delay/
                     * fanout=70). This loop's bound used to BE c_sz directly
                     * (`for (cc < c_sz)`), making the loop's own trip count
                     * -- not just a data value -- depend on that razor-thin
                     * timing path. csim (which just executes C in program
                     * order) can never see this; only real silicon can. This
                     * is board-confirmed as the root cause of DW's 46%
                     * mismatch (all of it landing on cc=1/dd=1, the second
                     * MAC_PD lane, which is exactly the lane whose write
                     * depends on the loop actually reaching a 2nd iteration).
                     * Fix, same technique as round 8's PW zero-fill: bound
                     * fixed at the compile-time constant MAC_PD, validity
                     * pushed into a plain data-path bool instead of loop
                     * control. Address clamped to a known-in-range constant
                     * (0, not min(oc,cout-1)) when invalid, so no runtime
                     * comparison is needed to keep the read in-bounds --
                     * the read result itself is discarded either way. */
                    /* A3 round (2026-08-22, ZHR-92, followup): pr/pc bound
                     * reverted from the compile-time PATCH_R_MAX/
                     * PATCH_C_MAX back to the runtime patch_r/patch_c --
                     * same recoverable-cost pattern as DW_WT_STAGE's
                     * MAX_K->K revert just below/above: at PATCH_R_MAX x
                     * PATCH_C_MAX=13x13=169/channel this loop iterates the
                     * worst-case (K=7,S=2) grid regardless of this layer's
                     * real K/S, measured 352 cycles for entry5 (K=3,S=1,
                     * real 6x6=36/channel) -- 338 of 338 total iterations
                     * run, only 72 needed.
                     * Protection mechanism verified term-by-term, not
                     * assumed (this is what makes reverting safe, same as
                     * DW_WT_STAGE's dd case): GATHER_ALL_DW's
                     * valid=(kh<dw_K)&&(kw<dw_K) mask sits on the WEIGHT
                     * side only --
                     *   lane_w_all[step][dd]  = valid ? dw_wtile[...] : 0;
                     *   lane_in_all[step][dd][rr][cw] = dw_patch[dd][rr*S+kh][cw*S+kw];  // no valid check here
                     * -- so for any kh>=dw_K (equivalently pr=rr*S+kh
                     * beyond this layer's real patch_r), the corresponding
                     * weight is forced to 0 and acc += lane_in*0 stays 0
                     * regardless of what dw_patch holds there. The read
                     * index's own upper bound is (MAC_PR-1)*S+(MAX_K-1),
                     * i.e. up to PATCH_R_MAX-1 -- for K=3/S=1 that's index
                     * 9, while this loop now only WRITES up to index
                     * patch_r-1=5; indices 6..9 stay uninitialized
                     * registers, but every step that reads them has
                     * kh>=K=3, so its weight is 0 and the read value never
                     * reaches acc. Holds BECAUSE dw_patch's declared size
                     * stays PATCH_R_MAX x PATCH_C_MAX (only the loop bound
                     * changed, not the array) -- shrinking the declaration
                     * itself would make those same reads genuinely
                     * out-of-bounds instead of merely unwritten. DO NOT
                     * shrink dw_patch's declared dimensions. */
                    /* A3 round (2026-08-23, ZHR-92, accumulator rewrite):
                     * see PW_PATCH_HOIST's header comment for the full
                     * rationale (same round, same mechanism, five sites).
                     * ch_off replaces `c * d.in_ch_stride` -- seeded once
                     * per (rt,colt,ot) at cc=0's value (one multiply,
                     * versus the ORIGINAL up to patch_r*patch_c=169
                     * re-evaluations of the same multiply per cc, since it
                     * used to sit inside the pr/pc nest), then advanced by
                     * a plain add per cc step. When cc is invalid (c_sz=1,
                     * only possible at cc=MAC_PD-1), ch_off's post-add
                     * value no longer matches the old code's clamp-to-0
                     * behavior -- harmless, because the array read that
                     * would use it is gated by `valid` in the SAME way the
                     * old code gated it (v stays 0, in_base is never
                     * actually read), same reasoning already established
                     * for DW_WT_STAGE's kh/kw revert elsewhere in this
                     * file: an invalid index feeding a value that's
                     * discarded, not an out-of-bounds read. `c` itself is
                     * no longer needed (was only ever used in this one
                     * multiply). */
                    /* A3 shared-multiplier round (2026-08-25, ZHR-92, U2598
                     * candidate #2) -- ATTEMPTED AND REVERTED. Hoisting
                     * MAC_PD*d.in_ch_stride outside this ot loop (a shift,
                     * MAC_PD compile-time) and accumulating ch_off_base by
                     * that step each ot -- same in_ch_base-style pattern
                     * already used one level down in this same loop -- held
                     * csim 17/17 but did NOT change mul_32s_32s_32_2_1's
                     * real instance count in the exported RTL (still 2,
                     * combined with candidate #1 above, tested together).
                     * Ruled out. */
                    int ch_off = ot * MAC_PD * d.in_ch_stride;
                    DW_PATCH_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                        bool valid = (cc < c_sz);
                        for (int pr = 0; pr < patch_r; pr++) {
                            int ih = rt * MAC_PR * S - P + pr;
                            for (int pc = 0; pc < patch_c; pc++) {
                                int iw = colt * MAC_PC * S - P + pc;
                                act_t v = 0;
                                if (valid &&
                                    ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                    v = in_base[d.in_off + ch_off + ih * Win + iw];
                                dw_patch[cc][pr][pc] = v;
                            }
                        }
                        ch_off += d.in_ch_stride;
                    }
                }

                for (int f = 0; f < n_f; f++) {
                    if (d.op_type == LDESC_OP_DWCONV) {
                        /* A3 round (2026-08-22, ZHR-92): split from one
                         * combined cc/kh/kw loop into two single-master
                         * loops. Root cause (confirmed via the exact
                         * csynth log message, not inferred): DW_WT_STAGE
                         * used to read BOTH gmem_b (dw_btile, at the cc
                         * level) and gmem_w (dw_wtile, at the kh/kw level)
                         * inside the same pipelined region. HLS's
                         * scheduler couldn't build a continuous request
                         * stream for gmem_w across that misaligned nesting
                         * -- it degraded to one full round-trip per
                         * access ("Unable to schedule bus request
                         * operation ... due to limited memory ports",
                         * achieved II=49, not the target 1). gmem_act's
                         * structurally-identical read pattern (kw-innermost,
                         * monotonic) bursts fine because it's never sharing
                         * a pipelined region with a second bundle; PW_WSTAGE
                         * (gmem_w alone, no second master) reaches II=1 as
                         * the natural control -- same read shape, same
                         * bundle, single-master loop. */
                        DW_BT_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                            bool valid = (cc < c_sz);
                            int c  = ot * MAC_PD + cc;
                            int oc = c * d.fpg + f;
                            int oc_safe = valid ? oc : 0;
                            dw_btile[cc] = valid ? b_base[d.b_off + oc_safe] : (acc_t)0;
                        }
                        /* A3 round (2026-08-22, ZHR-92, followup): kh/kw
                         * bound reverted from the compile-time MAX_K back
                         * to the runtime K -- MAX_K's own justification
                         * ("burst inference needs a compile-time trip
                         * count") stopped holding once this round confirmed
                         * gmem_w never actually bursts even as a
                         * single-master loop at II=1. At MAX_K, this loop
                         * does 2*49=98 iterations (measured 116 cycles);
                         * at K, it's 2*K*K -- 18 for this layer's K=3,
                         * measured ~36 cycles -- saving ~80 cycles/tile
                         * (11.5% of the 694-cycle tile total) for a
                         * property (burst eligibility) that was never
                         * actually achieved. This DOES reopen the
                         * "runtime bound into a complete-partitioned
                         * array" pattern the code review flagged -- but
                         * this specific instance was already identified as
                         * one of the four sites that were "accidentally
                         * safe": GATHER_ALL_DW's own
                         * valid=(kh<dw_K)&&(kw<dw_K) mask zeroes any
                         * kh>=K/kw>=K garbage this loop leaves in
                         * dw_wtile before it can reach the reduction.
                         * DO NOT remove that mask without re-closing this
                         * loop bound at the same time. */
                        /* A3 round (2026-08-23, ZHR-92, accumulator
                         * rewrite): same technique as DW_PATCH_STAGE just
                         * above -- wt_base replaces `oc_safe * K * K`
                         * (the outer, channel-indexed component of
                         * `(oc_safe*K+kh)*K+kw`; the kh*K+kw inner term is
                         * unrelated to this round's diagnosis -- a
                         * different multiply, by K not by a channel index,
                         * left as-is). Seeded once per (ot,f) at cc=0's
                         * value, advanced by the loop-invariant step
                         * d.fpg*K*K per cc. Same invalid-cc reasoning as
                         * DW_PATCH_STAGE: the read is gated by `valid`, so
                         * wt_base's un-clamped value when invalid is never
                         * actually dereferenced. */
                        int wt_base = (ot * MAC_PD * d.fpg + f) * K * K;
                        int wt_step = d.fpg * K * K;
                        DW_WT_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                            bool valid = (cc < c_sz);
                            for (int kh = 0; kh < K; kh++)
                                for (int kw = 0; kw < K; kw++) {
                                    dw_wtile[cc][kh][kw] = valid
                                        ? w_base[d.w_off + wt_base + kh * K + kw]
                                        : (wt_t)0;
                                }
                            wt_base += wt_step;
                        }
                    }

                    acc_t acc[MAC_PD][MAC_PR][MAC_PC];
                    #pragma HLS ARRAY_PARTITION variable=acc complete dim=0
                    RESET: for (int d0 = 0; d0 < MAC_PD; d0++)
                        for (int r0 = 0; r0 < MAC_PR; r0++)
                            for (int c0 = 0; c0 < MAC_PC; c0++) {
                                #pragma HLS UNROLL
                                acc[d0][r0][c0] = 0;
                            }

                    if (d.op_type == LDESC_OP_DWCONV) {
                        /* A3 round (2026-08-23, ZHR-92, run_layer rewrite
                         * stage 2): pw_wtile_dummy and the PW-shaped call
                         * args are GONE -- run_reduce_dw (renamed from
                         * run_reduce_unified) no longer takes op_type or
                         * any PW parameter at all, since PW no longer
                         * shares this function (see its own header
                         * comment for why -- pw_flat_pipeline handles PW
                         * entirely separately now). */
                        run_reduce_dw(MAX_K * MAX_K, dw_patch, dw_wtile, K, S, acc);
                    }
                    /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage
                     * 2): PW's cbase loop (PW_WSTAGE + run_reduce_unified,
                     * chunking Cin into MAX_CIN_PW pieces) that used to live
                     * in an `else` here is GONE -- this whole (ot,f) loop
                     * is now DW-only (see the outer `if (op_type==DWCONV)`
                     * wrap around the (rt,colt)-level ot loop). PW's
                     * equivalent logic lives in pw_flat_pipeline now,
                     * called once per (rt,colt) tile, not once per (ot,f).
                     * Leaving PW's old branch here would have been
                     * unreachable dead code -- op_type is guaranteed
                     * DWCONV at this point by the outer wrap, but HLS
                     * cannot prove that across the branch boundary on its
                     * own, so it would still have synthesized real,
                     * permanently-unused hardware for it (the exact
                     * "op_type is a runtime int at the call site even
                     * though this caller always passes one value" lesson
                     * already applied to run_reduce_unified's own PW
                     * branch in the earlier pw_one_ot round). */

                    /* A3 round 2 (2026-08-21, ZHR-92): a second attempt at
                     * hoisting/lookup-table-izing the WRITEOUT address
                     * arithmetic was TRIED and MEASURED WORSE (DSP
                     * 113->118, LUT 42616->42863) -- reverted to this
                     * simpler form, which is the actual measured-better
                     * state. Left as a cautionary note, not silently
                     * dropped: the lookup-table machinery's own cost
                     * (extra adders/registers building rr_row_off/dd_off
                     * each WRITEOUT call) exceeded whatever it saved,
                     * meaning HLS was likely already sharing/reusing the
                     * original per-idx multiply hardware across pipeline
                     * iterations reasonably well -- confirms this is where
                     * the address-arithmetic optimization line stops
                     * being worth pursuing further (per the round's own
                     * stop-loss criterion), not a bug in the attempt. */
                    if (d.op_type == LDESC_OP_DWCONV) {
                        /* A3 round (2026-08-23, ZHR-92, accumulator
                         * rewrite): WRITEOUT_DW is PIPELINE II=1 with dd
                         * flattened into idx (idx = dd*MAC_PR*MAC_PC + ...),
                         * so a simple "add once per outer-loop step"
                         * accumulator doesn't apply directly the same way
                         * as DW_PATCH_STAGE/DW_WT_STAGE's real nested
                         * loops -- dd only takes MAC_PD (2) distinct values
                         * across the whole pipelined loop, so instead
                         * precompute both values into small complete-
                         * partitioned tables BEFORE the pipelined loop
                         * (built via the same accumulator technique, just
                         * unrolled over MAC_PD steps instead of a real
                         * sequential loop), then the pipelined body does a
                         * table lookup (a mux) instead of a multiply.
                         * oc_tbl replaces `c*d.fpg+f`; oc_ch_tbl replaces
                         * `oc*d.out_ch_stride`.
                         *
                         * An outer-hoisted version of this (one table for
                         * ALL (ot,f), computed once per run_layer call
                         * instead of once per (rt,colt,ot,f)) was tried
                         * and reverted the same day -- grew the design
                         * enough to fail P&R routing, and the underlying
                         * "256x redundant multiply" premise it was
                         * chasing didn't hold up under scrutiny (two
                         * 32-bit multiplies cost 3-4 cycles, not the
                         * measured +422 cycles/tile). This per-call form
                         * is the last known-working state (96.00ms board,
                         * P&R routes) while the real +422 cycles/tile
                         * source is investigated properly -- current
                         * leading candidate is WRITEOUT_DW's own
                         * Interval=33 multiplied by its real per-tile call
                         * count, not this precompute block. */
                        int oc_tbl[MAC_PD], oc_ch_tbl[MAC_PD];
                        #pragma HLS ARRAY_PARTITION variable=oc_tbl complete dim=0
                        #pragma HLS ARRAY_PARTITION variable=oc_ch_tbl complete dim=0
                        {
                            int oc_val   = ot * MAC_PD * d.fpg + f;
                            int ch_val   = oc_val * d.out_ch_stride;
                            int ch_step  = d.fpg * d.out_ch_stride;
                            for (int dd0 = 0; dd0 < MAC_PD; dd0++) {
                                #pragma HLS UNROLL
                                oc_tbl[dd0]    = oc_val;
                                oc_ch_tbl[dd0] = ch_val;
                                oc_val += d.fpg;
                                ch_val += ch_step;
                            }
                        }
                        /* A3 round (2026-08-23, ZHR-92, WRITEOUT burst fix,
                         * REVERTED to single-path): the fast/slow dual path
                         * (see git history, commit 8f7856e) removed the
                         * per-element boundary `continue` and DID get past
                         * that specific diagnostic (AccessInCondBranchMissed
                         * gone from burst.xml), but a second, unnamed
                         * blocker remained (CouldNotAnalyzePatternMissed) --
                         * burst inference never actually fired, while the
                         * duplication itself cost +2,660 LUT (98%->103%,
                         * over budget). Paid the cost, didn't get the
                         * benefit -- reverted back to one guarded loop
                         * (2026-08-23, ZHR-92, decision to pursue a
                         * run_layer rewrite instead of chasing burst
                         * further). The genuinely independent win from that
                         * round is KEPT: shift no longer gets recomputed
                         * (or re-branched) every iteration -- it only takes
                         * MAC_PD distinct values across this whole call, so
                         * it's precomputed into a small partitioned table
                         * once, same technique as oc_tbl/oc_ch_tbl already
                         * use, before the (still guarded, non-bursting)
                         * loop runs. This is a real loop-invariant-hoist,
                         * not tied to the fast/slow split at all. */
                        int shift_tbl[MAC_PD];
                        #pragma HLS ARRAY_PARTITION variable=shift_tbl complete dim=0
                        for (int dd0 = 0; dd0 < MAC_PD; dd0++) {
                            #pragma HLS UNROLL
                            shift_tbl[dd0] = d.use_shift_table
                                ? (int)w_base[d.shift_off + oc_tbl[dd0]] : d.out_shift;
                        }
                        WRITEOUT_DW: for (int idx = 0; idx < MAC_PD * MAC_PR * MAC_PC; idx++) {
                            #pragma HLS PIPELINE II=1
                            int dd = idx / (MAC_PR * MAC_PC);
                            int rr = (idx / MAC_PC) % MAC_PR;
                            int cw = idx % MAC_PC;
                            if (dd >= c_sz || rr >= r_sz || cw >= col_sz) continue;
                            int oh = rt * MAC_PR + rr;
                            int ow = colt * MAC_PC + cw;
                            out_base[d.out_off + oc_ch_tbl[dd] + oh * d.w_out + ow] =
                                (act_t)clip_shift(acc[dd][rr][cw] + dw_btile[dd], shift_tbl[dd]);
                        }
                    }
                }
            }
            } else {
                /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2):
                 * PW's whole (ot,cbase) reduce + WRITEOUT_PW, replaced by
                 * one call -- see pw_flat_pipeline's own header comment
                 * above run_layer for the full rationale. Called once per
                 * (rt,colt) tile (unlike DW's per-ot loop above), since the
                 * flat pipeline handles every ot/cbase internally.
                 *
                 * ZHR-92 round (2026-08-27, PW_FLAT II=2->1): dispatch to
                 * the narrow (scalar-only) variant for the 2 real layers
                 * whose w_out isn't a multiple of MAC_PC (verified against
                 * layer_descriptor_256.json, see pw_flat_pipeline_impl's
                 * header comment) -- every other layer keeps the fast
                 * (burst-only) variant, now branch-free internally.
                 *
                 * FOUND AND FIXED via csim, not assumed correct: an
                 * earlier version of this condition also required
                 * n_col_tiles==1, which correctly covers the 2 real
                 * layers (both n_col_tiles==1) but incorrectly missed a
                 * layer with SEVERAL column tiles where only the LAST is
                 * a narrower remainder (w_out not an exact multiple of
                 * MAC_PC, n_col_tiles>1) -- the original per-tile
                 * fast_path check caught that case correctly (evaluated
                 * every tile), this layer-level dispatch must too.
                 * last_col_tile<MAC_PC alone is the right, general
                 * condition regardless of n_col_tiles; no real network
                 * layer has this mixed shape (verified) so this costs
                 * nothing on the deployed network, only broadens
                 * correctness for shapes the testbench exercises.
                 *
                 * SECOND bug found the same way (Phase16, in_off=3):
                 * the original fast_path was col_sz==MAC_PC AND
                 * byte_addr word-aligned -- col_sz alone was never
                 * sufficient (see pw_flat_pipeline_impl's own long-
                 * standing comment on this). byte_addr's alignment is
                 * (d.out_off + ot*d.out_ch_stride + ...) & 3; every term
                 * besides d.out_off is provably a multiple of 4 whenever
                 * w_out%MAC_PC==0 (out_ch_stride=h_out*w_out inherits
                 * w_out's divisibility; the (rt*MAC_PR+wr_row)*w_out and
                 * colt*MAC_PC terms are multiples of 4 by construction) --
                 * so on the non-narrow branch, alignment reduces to the
                 * layer-constant d.out_off&3, safe to check once here.
                 * GENERAL RULE (reusable beyond this one site): whenever a
                 * spatial dimension is known to be a multiple of MAC_PC/
                 * MAC_PR, every address term built from tile/row/col
                 * indices times that dimension is automatically a
                 * multiple of MAC_PC too -- only the layer-level base
                 * offset can break alignment, and it does so as a single
                 * per-layer constant, not something that varies by tile.
                 * Verified against the real deployed network (2026-08-27,
                 * layer_hw_sequence_256.json, all 82 sequence entries):
                 * zero PW entries have out_off%4!=0, so this alignment
                 * guard -- like the col_sz one above it -- costs nothing
                 * on the deployed network and exists purely to keep
                 * synthetic/future shapes correct. */
                bool pw_narrow = (d.last_col_tile < MAC_PC) || ((d.out_off & 3) != 0);
                if (pw_narrow) {
                    pw_flat_pipeline_narrow(d, w_base, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt, colt, r_sz, col_sz);
                } else {
                    pw_flat_pipeline(d, w_base, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt, colt, r_sz, col_sz);
                }
            }
        }
    }
}

/* ---- Phase A1 (2026-08-20): elementwise residual Add, two DRAM sources
 * (op0=in_off, op1=in2_off, confirmed from tools/layer_dag_ground_truth.json's
 * multi_input_nodes -- every real Add in the network reads the
 * token_mixer/identity branch and the layer_scale/processed branch),
 * one write. No MAC array involvement, so this being a separate function
 * with its own call site (see mac_array_top below) does NOT reintroduce
 * round 9/10's duplication problem -- that was specifically about two
 * callers needing the SAME expensive shared 64-wide MAC array; Add shares
 * no resource with run_layer's DW/PW path, there's nothing to duplicate.
 * Self-verified writeback (ZHR-91 row 6): exercised via
 * mac_array_tb.cpp's existing fault-injection harness pattern, extended
 * to target an Add layer specifically -- the historical defect-5 (Add
 * write-back failure on the old architecture, root cause never isolated)
 * is untested on this architecture until that check runs and passes. */
static void run_add(const LayerDescV2 &d,
                     const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    ADD: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        acc_t sum = (acc_t)in_base[d.in_off + i] + (acc_t)in_base[d.in2_off + i];
        out_base[d.out_off + i] = (act_t)clip_shift(sum, d.out_shift);
    }
}

/* ---- Phase A1 (SE block, priority 1 per user direction 2026-08-20):
 * GAP is the only operator in the whole design with a genuine full-tensor
 * read-before-write dependency -- every other op (DW/PW/Add/ReLU/Sigmoid/
 * Scale) can stream one output per some small constant number of reads.
 * GAP has to consume all H*W pixels of a channel before it can produce
 * that channel's single output value. This is the "does it expose
 * buffer/descriptor design gaps" case the whole SE-first ordering was
 * chosen for (see the interface sketch, ZHR-92: GAP flagged there as the
 * one op needing different layer-controller scheduling treatment, not a
 * new port). Division by H*W (not a power of 2 for arbitrary real layer
 * shapes) is a real integer divide here -- functionally correct for A1's
 * csim-level goal, but a synthesis-cost item to revisit later, not
 * solved now (matches the project's "don't optimize efficiency this
 * round" discipline). */
static void run_gap(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int HW = d.h_in * d.w_in;
    GAP_C: for (int c = 0; c < d.cin; c++) {
        acc_t sum = 0;
        GAP_HW: for (int i = 0; i < HW; i++) {
            #pragma HLS PIPELINE II=1
            sum += (acc_t)in_base[d.in_off + c * HW + i];
        }
        acc_t avg = sum / HW;
        out_base[d.out_off + c] = (act_t)clip_shift(avg, d.out_shift);
    }
}

static void run_relu(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    RELU: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        act_t v = in_base[d.in_off + i];
        out_base[d.out_off + i] = (v < 0) ? act_t(0) : v;
    }
}

/* PLACEHOLDER quantized sigmoid -- a crude monotonic saturating linear
 * map, NOT calibrated against any real sigmoid curve. Proves the
 * GAP->fc1->ReLU->fc2->Sigmoid->Scale data flow wires together and
 * produces a plausible gate; numeric accuracy is explicitly out of scope
 * for A1 (that's calibration/quantization work, later). Flagging this
 * now rather than letting it silently become a landmine -- this project
 * has already been burned once by an unflagged placeholder
 * (calibrate_activations.py's default_act_scale, 37x off, undiscovered
 * for 9+ rounds). Reused (not shared code, but the same intent) by
 * run_gelu below -- GELU(x)~=x*sigmoid(1.702x) is a standard documented
 * approximation, so using this same placeholder gate for both keeps the
 * two "uncalibrated" surfaces consistent instead of inventing a second,
 * unrelated placeholder shape for GELU. */
static act_t quantized_sigmoid(act_t x)
{
    acc_t v = (acc_t)x + 64;
    if (v > 127) v = 127;
    if (v < 0)   v = 0;
    return (act_t)v;
}

static void run_sigmoid(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    SIGMOID: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        out_base[d.out_off + i] = quantized_sigmoid(in_base[d.in_off + i]);
    }
}

/* GELU, single source (op0 only) -- real ONNX pattern is Div->Erf->Add->
 * Mul (17 instances, confirmed via direct node inspection, see ZHR-64);
 * this is the ATOMIC hardware op a folded descriptor entry dispatches to,
 * not a re-implementation of the 4-node decomposition. gelu(x)~=x*sigmoid
 * (1.702x), computed here as x*quantized_sigmoid(x) -- same placeholder-
 * accuracy caveat as run_sigmoid, proves data flow not numeric fidelity.
 * tools/gen_gelu_lut.py exists as the real calibrated asset to integrate
 * when this needs actual accuracy (not this round). */
static void run_gelu(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    GELU: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        act_t x = in_base[d.in_off + i];
        acc_t prod = (acc_t)x * (acc_t)quantized_sigmoid(x);
        out_base[d.out_off + i] = (act_t)clip_shift(prod, d.out_shift);
    }
}

/* SE's final gate multiply: op0 (in_off) is the full HxWxC feature map,
 * op1 (in2_off) is the C-length gate, broadcast over spatial -- confirmed
 * from tools/layer_dag_ground_truth.json: final_conv's node has fan_out=2,
 * feeding both ReduceMean AND this Mul directly from the same tensor. */
static void run_scale(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int HW = d.h_in * d.w_in;
    SCALE_C: for (int c = 0; c < d.cin; c++) {
        act_t gate = in_base[d.in2_off + c];
        SCALE_HW: for (int i = 0; i < HW; i++) {
            #pragma HLS PIPELINE II=1
            acc_t prod = (acc_t)in_base[d.in_off + c * HW + i] * (acc_t)gate;
            out_base[d.out_off + c * HW + i] = (act_t)clip_shift(prod, d.out_shift);
        }
    }
}

/* A3 interface resource-budget check (2026-08-21, ZHR-92). Round 5-15's
 * resource numbers are compute-core-only (HW Interfaces section confirmed
 * every port ap_none/ap_vld, no #pragma HLS INTERFACE anywhere) -- this
 * is the first csynth run with a real m_axi interface, specifically to
 * get a real number before committing to any further A3 implementation.
 * 4 masters, matching the old architecture's proven 17->4 consolidation
 * (ZHR-8) rather than one bundle per array: gmem_act shared by in_base/
 * out_base (already the SAME physical buffer under Route C -- the ARM
 * writes Stem's output and reads the final result from the same region),
 * gmem_w for weights (includes the appended per-channel shift table),
 * gmem_b for bias, gmem_meta for desc+out_written (small, low-bandwidth
 * control/descriptor traffic, doesn't need its own high-bandwidth path).
 * desc stays ap_none-shaped in this signature deliberately -- the
 * reviewed interface sketch's DRAM-resident, read-one-entry-at-a-time
 * `desc` table is real A3 work, not needed just to get a first resource
 * number; m_axi on the same struct-array parameter is enough to see
 * whether AXI infrastructure fits the budget at all before doing that. */
/* ZHR-92 (2026-08-30): desc/out_written moved OFF gmem_meta (m_axi) onto
 * s_axilite, mirroring mac_array_raster_integrated.cpp's own change
 * (2026-08-29) -- mac_array.cpp shares mac_array_top's declaration in
 * mac_array.h with the raster file, so this update is required just to
 * keep this file buildable as the golden-rollback reference, not a new
 * independent decision. Same rationale as the raster file's own header
 * comment: n_layers is ALWAYS 1 on every real dispatch. */
void mac_array_top(
    LayerDescV2 desc,
    const act_t  in_base[],
    const wt_t   w_base[],
    const acc_t  b_base[],
    act_t        out_base[],
    int          *out_written,
    const ap_uint<32> in_base_wide[],
    hls::burst_maxi<ap_uint<32> > out_burst,
    hls::burst_maxi<ap_uint<32> > in_burst)
{
#pragma HLS INTERFACE s_axilite port=desc     bundle=control
#pragma HLS INTERFACE m_axi port=in_base      offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=w_base       offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=b_base       offset=slave bundle=gmem_b
#pragma HLS INTERFACE m_axi port=out_base     offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=out_written bundle=control
/* ZHR-92 angle-B step (2026-08-24): out_burst is hls::burst_maxi<ap_uint
 * <32>>, matching gmem_act's real 32-bit AXI width (forced by
 * in_base_wide already sharing this bundle) -- the previous attempt used
 * <act_t> (8-bit) and crashed csynth's codegen. Same bundle=gmem_act, not
 * a 5th master. Used only by pw_flat_pipeline's WRITEOUT; the other 7
 * out_base call sites (WRITEOUT_DW, run_add/gap/relu/sigmoid/gelu/scale)
 * are untouched, still plain-pointer out_base. */
#pragma HLS INTERFACE m_axi port=out_burst    offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=out_burst bundle=control
/* A3 row-hoist round (2026-08-25, ZHR-92): in_burst is ROW_READ's read-side
 * counterpart to out_burst, same bundle=gmem_act, same "shares a bundle,
 * does NOT share a control register" caveat as out_burst's own history --
 * this gets its own AXI-Lite base-address register, which the host driver
 * must program explicitly before dispatch (not yet wired into
 * mac_array_driver.c at this stage -- csim has no register-address concept
 * and cannot catch a missing write; this is a P&R-stage TODO, tracked, not
 * silently deferred). */
#pragma HLS INTERFACE m_axi port=in_burst     offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=in_burst bundle=control
/* A3 round (2026-08-23, ZHR-92, MERGE): back on bundle=gmem_act, sharing
 * the SAME physical master as in_base/out_base -- the standalone
 * gmem_act_wide master (previous round, solution18) is gone. That
 * earlier round's justification for a SEPARATE bundle no longer applies:
 * the "limited memory ports" II violation it was avoiding happened when
 * TWO DIFFERENT bundles were both accessed inside ONE pipelined loop
 * region (wide/narrow branch INSIDE the loop, both statically scheduled
 * even though only one runs). That branch is gone now too (see
 * PW_PATCH_HOIST above) -- in_base_wide is PW_PATCH_HOIST's ONLY
 * accessor, in its own loop region, so this is architecturally the same
 * "one bundle, multiple ports, never contended within one region" shape
 * run_add already uses today (in_base x2 + out_base, all bundle=gmem_act,
 * inside ONE pipelined ADD loop, uncontested) -- not a new pattern, reusing
 * a load-bearing one already proven in this file. Motivation: the
 * standalone master's own HLS-side adapter cost 657 LUT/671 FF/2 BRAM18K
 * by itself (~30% of that round's total LUT growth), before counting the
 * BD-level SmartConnect's added crossbar port -- removing it is expected
 * to both shrink LUT and return the AXI master count to 4. Whether it
 * also resolves the FSM->DSP critical path (see PW_PATCH_HOIST's comment)
 * is the thing THIS round's P&R actually tests, not assumed here. */
#pragma HLS INTERFACE m_axi port=in_base_wide offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=in_base bundle=control
#pragma HLS INTERFACE s_axilite port=w_base bundle=control
#pragma HLS INTERFACE s_axilite port=b_base bundle=control
#pragma HLS INTERFACE s_axilite port=out_base bundle=control
#pragma HLS INTERFACE s_axilite port=in_base_wide bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control
    /* A3 round (2026-08-22, ZHR-92): option D (read desc[i] into a local
     * on-chip copy once per layer, on the theory that gmem_meta's critical
     * path was caused by scattered per-field AXI reads/high fan-out) was
     * ATTEMPTED AND REVERTED -- placement failed outright (real LUT 66,317
     * vs 53,200 available, 25% over capacity; the whole-struct assignment
     * synthesized real burst-FIFO/decode logic, visible even at csynth as
     * a BRAM_18K jump from 31 to 141). More importantly the diagnosis
     * behind it was wrong: the first P&R run's own record already said
     * "zero logic levels, 74% routing delay, fanout=70" -- fanout=70 is
     * not high for a 704-bit bus reaching all of run_layer (expected
     * hundreds+), and "zero logic levels + 74% routing" describes a
     * PLACEMENT DISTANCE problem (gmem_meta's AXI interface logic placed
     * physically far from run_layer's registers), not a fan-out problem.
     * That record was available and cited before option D was designed,
     * just not read carefully enough. See option E (pblock) for the actual
     * distance-targeted fix -- SUPERSEDED 2026-08-30: gmem_meta itself is
     * gone now (desc/out_written moved to s_axilite, see the function-
     * header comment above), so this whole distance-to-run_layer problem
     * is moot for desc, not just mitigated. */
    switch (desc.op_type) {
        case LDESC_OP_ADD:     run_add(desc, in_base, out_base); break;
        case LDESC_OP_GAP:     run_gap(desc, in_base, out_base); break;
        case LDESC_OP_RELU:    run_relu(desc, in_base, out_base); break;
        case LDESC_OP_SIGMOID: run_sigmoid(desc, in_base, out_base); break;
        case LDESC_OP_SCALE:   run_scale(desc, in_base, out_base); break;
        case LDESC_OP_GELU:    run_gelu(desc, in_base, out_base); break;
        default:                run_layer(desc, in_base, w_base, b_base, out_base, in_base_wide, out_burst, in_burst); break;
    }
    *out_written = 1;
}
