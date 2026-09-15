#include "mac_array.h"
// mac_array_raster_integrated.cpp -- ZHR-92 Phase 1 Step 2 P&R round
// (2026-08-28). Verbatim copy of mac_array.cpp with exactly ONE change:
// run_layer's very first statement is now an early-return DW branch to
// run_dw_layer_raster() (the new raster mechanism, csim-verified 5/5
// against real layer data, csynth-verified II=2/resource-measured in
// isolation). Everything below that branch -- every PW-specific line,
// every shared helper, mac_array_top itself -- is untouched, byte-
// identical to mac_array.cpp; DW's old inline mechanism (ROW_READ_FILL,
// DW_PATCH_STAGE, DW_WT_STAGE, WRITEOUT_DW, run_reduce_dw) becomes dead
// code for DW ops (never reached, since DW returns before reaching it)
// but is left in place, not deleted, so a byte-for-byte diff against
// mac_array.cpp outside the new branch stays a meaningful "PW untouched"
// proof. This file exists ONLY to give this round's P&R a real, whole-IP
// context (WNS, real critical path) instead of measuring
// run_dw_layer_raster in isolation -- mac_array.cpp itself remains the
// untouched golden rollback.
#include "dw_raster_layer.h"

/* ZHR-92 (2026-09-15): PW_DEFER_WRESP is ON BY DEFAULT as of the
 * mac_array_a3_pwdefer deployed baseline (real board: PW 495.1 -> 306.6ms,
 * -38%; full network ~679 -> ~496ms, -27%; byte-exact, ONNX cosine exact).
 * PW_FLAT's write_response() is deferred to the ot two back (popped at
 * writeout row starts once >= 8 are pending; <= 8 in flight vs the
 * adapter's 16), so no output word waits for its own B response inside the
 * II=1 pipeline any more -- the real-board fit's 18-cycle-per-word term
 * (163ms) went to ~0. Request and write are unchanged. Define
 * PW_DEFER_WRESP_OFF to get the in-iteration response back. See
 * pw_flat_pipeline_impl's own comment. */
#ifndef PW_DEFER_WRESP_OFF
#define PW_DEFER_WRESP 1
#endif

/* PW_ROWREAD_PREFETCH prefetch depth (requests in flight), sized against the
 * gmem_act adapter: NUM_READ_OUTSTANDING 16, request FIFO 16, read-data
 * buffer 256 words; real PW rows <= 16 words -> 8 in flight = 128 words. */
#ifndef PW_ROWREAD_PF
#define PW_ROWREAD_PF 8
#endif

/* ZHR-92 (2026-09-15): PW_ROWREAD_PREFETCH is ON BY DEFAULT as of the
 * mac_array_a3_pwpf deployed baseline (real board: PW 307 -> 231ms, -25%;
 * full network ~496 -> ~418ms, -16%; byte-exact, ONNX cosine exact). The
 * real-board fit's per-ROW_READ-request term went 71 -> 34 cycles; the
 * remaining ~34 is ROW_READ_FILL's fixed MAX_WORDS_PER_CH trip count +
 * loop overhead, not AXI latency. Define PW_ROWREAD_PREFETCH_OFF to get the
 * serial request-then-fill form back. */
#ifndef PW_ROWREAD_PREFETCH_OFF
#define PW_ROWREAD_PREFETCH 1
#endif

/* ZHR-92 (2026-09-15): PW_ROWREAD_MERGE4 is ON BY DEFAULT as of the
 * mac_array_a3_merge4 deployed baseline (real board: PW 231 -> 193ms, -16%;
 * full network ~418 -> ~378ms, -9.5%; byte-exact, ONNX cosine exact). One
 * ROW_READ request per (rt, ci) covering the tile's 4 contiguous input rows
 * (requests 179,904 -> 44,976), runtime FILL trip count, explicit-bank
 * (word<<2)|lane store indexing (FILL II=1), per-layer W%4==0 && aligned guard
 * -- the W=1 SE fc layers and any synthetic W<4 shape take the original
 * per-(rr,ci) path, which stays compiled in. Define PW_ROWREAD_MERGE4_OFF to
 * disable. */
#ifndef PW_ROWREAD_MERGE4_OFF
#define PW_ROWREAD_MERGE4 1
#endif
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
/* ZHR-92 round (2026-09-02): PW_CACHED downgraded from a 2nd template bool
 * to a plain runtime bool -- see PW_WEIGHT_HOIST's own header comment for
 * why (4 independently-synthesized instantiations, +21.2% LUT and a
 * doubled pw_weight_cache BRAM footprint, neither shared). This is a
 * DIFFERENT case from FAST_WRITEOUT's own template dispatch: that one
 * hoists a runtime branch specifically because two mutually-exclusive
 * branches were found WRITING to the SAME shared AXI port (gmem_act),
 * which the scheduler couldn't prove non-conflicting across iterations,
 * forcing II=2 (see FAST_WRITEOUT's own comment). PW_CACHED's two arms are
 * a BRAM read (pw_weight_cache, no AXI port at all) vs. an AXI READ on
 * gmem_w -- only one arm ever touches gmem_w, so there is no second
 * branch's access to the same port for the scheduler to reconcile. This is
 * inference from the diagnosed FAST_WRITEOUT mechanism, not an assumption
 * to skip verifying: confirm PW_FLAT's own achieved II is still 1 (both
 * FAST_WRITEOUT instances) before trusting this. */
template<bool FAST_WRITEOUT>
static void pw_flat_pipeline_impl(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const wt_t pw_weight_cache[],
    bool pw_cached,
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    hls::burst_maxi<ap_uint<32> > &out_burst,
    int rt_row_base, int colt, int r_sz, int col_sz,
    int ot_start, int ot_count, int ot_out_ch_base_init, int total_iters_in)
{
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic factor=MAC_PD dim=1
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3

    const int Cin = d.cin;
    /* ZHR-92 round (2026-09-03): chunked weight loading -- ot_start/
     * ot_count restrict this call to a SUBRANGE of d.cout (the ot's whose
     * weight is currently resident in pw_weight_cache for this chunk), not
     * necessarily the full layer. w_ot_base (the cache-relative gather
     * index, below) stays 0-based regardless of ot_start -- the cache
     * itself only ever holds ot_count rows starting from its own index 0,
     * never the full-layer absolute range. ot_idx (which indexes
     * pw_bias_cache/pw_shift_cache) starts at the ABSOLUTE ot_start
     * instead (a plain assignment, not a multiply -- see below for why
     * ot_out_ch_base is handled differently). Degenerate case (whole layer
     * fits in one chunk): ot_start=0, ot_count=d.cout, bit-identical to
     * the pre-chunking call shape.
     *
     * ZHR-92 round (2026-09-03, shared-multiplier regression fix): a first
     * version of this chunking change computed `ot_out_ch_base = ot_start
     * * d.out_ch_stride` HERE, inside this function's own init -- real P&R
     * came back WNS=-2.264415ns (vs the pre-chunking baseline's
     * +0.153200ns), a large real violation. The critical path (pulled and
     * read directly, not guessed) traced to THIS function's own
     * (pw_flat_pipeline_impl_false, the narrow instance) FSM state feeding
     * mul_32s_32s_32_2_1 -- the SAME shared physical multiplier this file's
     * own "shared-multiplier round" comments already document (multiple
     * `index*stride`-shaped address computations across run_layer bound to
     * ONE physical multiplier, arbitrated by FSM state, each added call
     * site deepening the selection tree). This function is called once per
     * (rt,colt) spatial tile -- up to 4x per chunk for the real fallback-
     * layer shapes -- so the multiply this used to compute here executed
     * far more often than necessary, AND from inside this function's own
     * separately-synthesized FSM (physically farther from the shared
     * multiplier than run_layer's own top-level call sites), explaining
     * why this regression (-2.264ns) was far larger than every prior
     * instance of this same mechanism (historically -0.1 to -0.3ns).
     * Fixed by moving the multiply to run_layer's own PW_WCHUNK loop (a
     * genuine loop-carried accumulator there, stepped once per CHUNK, not
     * once per spatial tile) and passing the already-computed result in
     * as ot_out_ch_base_init -- this function now only ASSIGNS it, no
     * multiply here at all. See PW_WCHUNK's own comment in run_layer for
     * the accumulator itself.
     *
     * ZHR-92 round (2026-09-03, SECOND shared-multiplier fix, same round):
     * the ot_out_ch_base fix above only halved the regression (WNS
     * -2.264415ns -> -1.127835ns, still negative) -- the real critical-
     * path report (pulled again, not guessed) showed the SOURCE had moved
     * to run_layer's OWN top-level FSM state (not this function's own
     * separate FSM anymore), still driving into the SAME shared
     * mul_32s_32s_32_2_1, but the SINK this time was `total_iters`'s own
     * multiply (`n_ot * (n_cbase*PW_FLAT_STEPS_PER_CBASE+...)`, directly
     * below) -- confirming PW_WCHUNK's new loop-nesting level had itself
     * restructured run_layer's FSM in a way that changed how it competes
     * for the shared multiplier, not just "two more multiplies exist."
     * total_iters is loop-invariant across the whole (rt,colt) spatial
     * sweep for a given chunk (n_ot=ot_count and n_cbase both fixed once a
     * chunk starts) -- exactly the same redundant-per-tile-recomputation
     * shape as ot_out_ch_base, so it gets the same fix: computed ONCE per
     * chunk in run_layer's own PW_WCHUNK loop (see there), passed in here
     * as total_iters_in. n_cbase itself STAYS computed locally below (it's
     * a division, needed again for the cbase-wrap comparison further down,
     * and was never implicated in either critical-path report -- only the
     * n_ot*(...) MULTIPLY moved). */
    const int n_cbase = (Cin + MAX_CIN_PW - 1) / MAX_CIN_PW;
    const int total_iters = total_iters_in;

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
    int w_ot_base = 0;           /* == (ot-ot_start)*Cin, cache-relative, accumulated from 0 every call */
    int ot_out_ch_base = ot_out_ch_base_init;  /* == ot*d.out_ch_stride, ABSOLUTE -- passed in, not multiplied here */
    int ot_idx = ot_start;       /* ABSOLUTE ot -- indexes pw_bias_cache/pw_shift_cache/DRAM addressing */
    int wr_row = 0, wr_col = 0;  /* writeout row/col -- wrap-pair, not idx/MAC_PC and idx%MAC_PC */
    int wr_row_off = 0;          /* == wr_row * d.w_out, ACCUMULATED alongside wr_row (SHARED_MUL_ARMS round, 2026-09-12) */
    int shift_reg = d.out_shift; /* recomputed at each ot's compute->writeout transition */
#ifdef PW_DEFER_WRESP
    /* ZHR-92 (2026-09-14/15) PW_DEFER_WRESP -- ON BY DEFAULT since 2026-09-15
     * (see the define after the includes; PW_DEFER_WRESP_OFF reverts),
     * deployed as mac_array_a3_pwdefer. History below as written during
     * the round. Originally a STEP-1 MECHANISM PROBE, OFF by
     * default. The deployed FAST_WRITEOUT issues write_request + write +
     * write_response for every 4-byte output word, all three inside ONE
     * PW_FLAT iteration (sched: writereq ST_9, write ST_10, 5-stage
     * writeresp ST_11-15 -- the IDENTICAL stage numbers DW's CROW_CCOL had
     * before DWR_ROWBURST), so every word waits ~18-30 cycles for its own B
     * response inside the II=1 pipeline: 907,056 words x 18 cycles = ~163ms
     * of PW's 495ms (real-board fit, R^2 0.992). Unlike DW, one tile's 4
     * rows are w_out apart -- a row burst does not apply; but the stall is
     * the RESPONSE wait, not the request count. This variant keeps the
     * request and the write exactly where they are (wr_col==MAC_PC-1) and
     * DEFERS the response: pop one write_response() at each writeout ROW
     * START (wr_col==0) only once >= 8 are pending, i.e. the responses of
     * the ot two back are collected during this ot's writeout rows. Gap
     * between a write and its pop >= 2*(n_cbase*8+16) - 4 >= 60 cycles for
     * every real cin -- longer than the B round trip -- so the pop should
     * find its response already there. Max in flight = 8 (4 of ot n-2 + 4
     * of ot n-1 at an ot boundary); the gmem_act adapter allows 16
     * (NUM_WRITE_OUTSTANDING = USER_MAXREQS = 16, read from the exported
     * RTL). The <= 8 responses still pending after the loop are drained by
     * PW_WRESP_DRAIN below (once per pw_flat_pipeline_impl call). This is
     * NOT the PW_WRITEOUT_FLUSH shape (2026-09-04, +142%): the data write
     * never leaves the pipeline. Step-1 judgment: in the PW_FLAT schedule,
     * writereq/write still inside at II=1, writeresp still inside but on a
     * DISJOINT predicate (wr_col==0 & pending>=8 vs wr_col==3) -- not
     * outside the loop the way DW's is, because here the deferral is to a
     * later ITERATION of the same loop. */
    ap_uint<5> w_pending = 0;    /* write_requests issued minus responses popped, <= 8 */
#endif

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
                /* ZHR-92 round (2026-09-02): PW_FIX_WADDR -- timing-only probe,
                 * values WILL be wrong. Forces every weight read to the same
                 * address (d.w_off, always in-range) instead of the real
                 * per-(ot,cbase,ch) address, with the read itself, its op
                 * count, and every surrounding structure left untouched --
                 * isolates "how much of PW's time is this address's own real
                 * DRAM traffic" from "how much is the read operation's fixed
                 * overhead regardless of address." Board-only, csim/
                 * checkpoint NOT meaningful under this flag. Off by default. */
                /* ZHR-92 round (2026-09-02): PW_CACHED is now a plain
                 * runtime bool, not a template parameter -- see this
                 * function's own header comment for why this is believed
                 * safe (only one arm touches gmem_w, unlike FAST_WRITEOUT's
                 * own same-port-two-writes trigger). See PW_WEIGHT_HOIST's
                 * header comment above run_layer for sizing/partitioning. */
                /* ZHR-92 round (2026-09-04): the uncached (gmem_w direct-
                 * read) arm below is DEAD on every real shape this project
                 * can construct -- pw_cached_ok (run_layer's own caller-
                 * side computation, see its header comment above) is
                 * `d.cin <= PW_WEIGHT_CACHE_ELEMS`, true for every real
                 * layer (max real cin=1152 vs cache=147,456) since
                 * PW_WCHUNK. Kept as a runtime `if(pw_cached)` (not a
                 * template, per the original design's own reasoning: "only
                 * one arm touches gmem_w, unlike FAST_WRITEOUT's own
                 * same-port-two-writes trigger") -- true when MAC_PD's own
                 * per-`dd` UNROLL had exactly 1 lane. That assumption
                 * silently stopped holding at MAC_PD=2: the unrolled `dd`
                 * loop now duplicates this WHOLE if/else across 2
                 * simultaneous lanes, and HLS must conservatively schedule
                 * for both lanes hitting the dead gmem_w arm in the same
                 * cycle -- confirmed via a direct compile-time-constant
                 * test (`if(true)`) that this, not real gmem_w bandwidth
                 * demand, is what regresses PW_FLAT's achieved II to 2 (see
                 * this round's own CLAUDE.md/Linear write-up). Default is
                 * now the compile-time-constant form (matches what
                 * pw_cached_ok's own value always evaluates to on real
                 * hardware); the old runtime-gated form is preserved,
                 * unused by default, behind PW_ALLOW_UNCACHED_FALLBACK for
                 * anyone who needs to re-enable the direct-DRAM path (e.g.
                 * a future cin > PW_WEIGHT_CACHE_ELEMS shape), per this
                 * project's own convention for prior dead-but-kept
                 * fallbacks (use_wide_path, PW_PATCH_HOIST's in_base_wide
                 * parameter). */
#ifdef PW_ALLOW_UNCACHED_FALLBACK
                if (pw_cached) {
#else
                if (true) {
#endif
                    /* ZHR-92 round (2026-09-03): PW_FIX_WCACHE_ADDR --
                     * timing-only probe, mirrors PW_FIX_WADDR's own
                     * discipline but for the NOW-LIVE cached-BRAM-read
                     * branch (PW_FIX_WADDR above only forces the DEAD
                     * direct-DRAM-read branch, unreachable on any real/
                     * tested shape since the PW_WCHUNK round -- see
                     * PW_WEIGHT_CACHE_ELEMS's own header comment). This
                     * round's own goal (ZHR-92, "opening B", re-decomposing
                     * PW's external-cost breakdown now that weight-hoist's
                     * old 55.3% component has been chunked away) needs a
                     * probe for THIS branch specifically -- pw_weight_cache
                     * index 0 is always in-range regardless of shape.
                     * Board-only, csim/checkpoint NOT meaningful under this
                     * flag. Off by default. */
#ifdef PW_FIX_WCACHE_ADDR
                    lane_w[dd] = ch_valid ? pw_weight_cache[0] : (wt_t)0;
#else
                    lane_w[dd] = ch_valid ? pw_weight_cache[w_ot_base + ch_off + dd] : (wt_t)0;
#endif
                } else {
#ifdef PW_FIX_WADDR
                    lane_w[dd] = ch_valid ? w_base[d.w_off] : (wt_t)0;
#else
                    lane_w[dd] = ch_valid ? w_base[d.w_off + w_ot_base + ch_off + dd] : (wt_t)0;
#endif
                }
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
                        acc_t prod;
                        /* ZHR-92 round (2026-09-01): PW_FORCE_DSP -- untested
                         * counterpart to DW's LB_FORCE_DSP (dw_raster_layer.cpp).
                         * Conservative variant of the CLOSED DSP-packing line:
                         * same MAC_PD=1/MAC_PR=4/MAC_PC=4=16-lane parallelism,
                         * same datapath, only rebinding this existing 1:1
                         * multiply from LUT to DSP -- no new gmem_w bandwidth
                         * demand, unlike the closed MAC_PD-expansion attempt.
                         * Off by default; measure isolated LUT release / DSP
                         * cost / II before adopting. */
#ifdef PW_FORCE_DSP
#pragma HLS BIND_OP variable=prod op=mul impl=DSP
#endif
                        prod = (acc_t)lane_in[dd][rr][cw] * (acc_t)lane_w[dd];
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
                /* ZHR-92 round (2026-09-03): PW_FIX_BIASADDR -- timing-only
                 * probe (see PW_FIX_WCACHE_ADDR's own comment above for the
                 * round this belongs to). pw_bias_cache/pw_shift_cache were
                 * never individually probed before -- this round's own goal
                 * is a full re-decomposition of PW's external cost now that
                 * weight-read's old 55.3% share is gone. Index 0 always
                 * in-range. Off by default. */
#ifdef PW_FIX_BIASADDR
                total += pw_bias_cache[0];
#else
                total += pw_bias_cache[ot_idx];
#endif
                val = (act_t)clip_shift(total, shift_reg);
            }
            /* A3 shared-multiplier round (2026-08-25, ZHR-92, U2598
             * candidate #1) -- ATTEMPTED AND REVERTED at the time: converting
             * (rt*MAC_PR+wr_row)*d.w_out to an rt-loop accumulator + wr_row
             * table held csim but "did NOT change mul_32s_32s_32_2_1's real
             * instance count (still 2)". THAT WAS THE WRONG METRIC (SHARED_
             * MUL_ARMS round, 2026-09-12): the instance count stays 2 as long
             * as ANY call site remains bound to the shared unit; the thing
             * that sets the sink's delay is the number of ARMS in its operand
             * mux (binding DB opset / the always@(*) block driving grp_fu_*),
             * and by that metric this multiply -- executed INSIDE the PW_FLAT
             * II=1 pipeline, once per writeout iteration, bound out through an
             * external FU port -- was the arm ON the critical path (FSM state
             * 122 -> mul_32s_32s_32_2_1, every thin-margin build on this
             * line). Re-done here as: rt_row_base (== rt*MAC_PR*w_out,
             * accumulated in run_layer's rt loop, passed in) + wr_row_off
             * (== wr_row*w_out, accumulated next to wr_row above). Zero
             * multiplies. Judged by the binding DB + WNS, not instance count. */
            if (wr_row < r_sz) {
                /* ZHR-92 round (2026-09-02): PW_FIX_OUTADDR -- timing-only
                 * probe, mirrors PW_FIX_WADDR exactly (see its comment
                 * above). d.out_off is always in-range. Off by default. */
#ifdef PW_FIX_OUTADDR
                int byte_addr = d.out_off;
#else
                int byte_addr = d.out_off + ot_out_ch_base + rt_row_base + wr_row_off + colt * MAC_PC;
#endif
                if (FAST_WRITEOUT) {
                    row_word.range(wr_col * 8 + 7, wr_col * 8) = val;
#ifdef PW_DEFER_WRESP
                    if (wr_col == 0 && w_pending >= 8) {
                        out_burst.write_response();   /* response of the ot two back */
                        w_pending--;
                    }
                    if (wr_col == MAC_PC - 1) {
                        out_burst.write_request(byte_addr >> 2, 1);
                        out_burst.write(row_word);
                        w_pending++;
                    }
#else
                    if (wr_col == MAC_PC - 1) {
                        out_burst.write_request(byte_addr >> 2, 1);
                        out_burst.write(row_word);
                        out_burst.write_response();
                    }
#endif
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
#ifdef PW_FIX_BIASADDR
                    shift_reg = d.use_shift_table ? (int)pw_shift_cache[0] : d.out_shift;
#else
                    shift_reg = d.use_shift_table ? (int)pw_shift_cache[ot_idx] : d.out_shift;
#endif
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
                wr_row_off = 0;
            }
        } else {
            k++;
            if (!in_writeout) {
                ch_off += MAC_PD;
            } else {
                if (wr_col == MAC_PC - 1) { wr_col = 0; wr_row++; wr_row_off += d.w_out; }
                else { wr_col++; }
            }
        }
    }
#ifdef PW_DEFER_WRESP
    if (FAST_WRITEOUT) {
        PW_WRESP_DRAIN: for (int r = 0; r < (int)w_pending; r++) {
            out_burst.write_response();
        }
    }
#endif
}

static void pw_flat_pipeline(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const wt_t pw_weight_cache[],
    bool pw_cached,
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    hls::burst_maxi<ap_uint<32> > &out_burst,
    int rt_row_base, int colt, int r_sz, int col_sz,
    int ot_start, int ot_count, int ot_out_ch_base_init, int total_iters_in)
{
    pw_flat_pipeline_impl<true>(d, w_base, pw_weight_cache, pw_cached, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt_row_base, colt, r_sz, col_sz, ot_start, ot_count, ot_out_ch_base_init, total_iters_in);
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
    const wt_t pw_weight_cache[],
    bool pw_cached,
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    hls::burst_maxi<ap_uint<32> > &out_burst,
    int rt_row_base, int colt, int r_sz, int col_sz,
    int ot_start, int ot_count, int ot_out_ch_base_init, int total_iters_in)
{
    pw_flat_pipeline_impl<false>(d, w_base, pw_weight_cache, pw_cached, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt_row_base, colt, r_sz, col_sz, ot_start, ot_count, ot_out_ch_base_init, total_iters_in);
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
                       hls::burst_maxi<ap_uint<32> > &in_burst,
                       hls::burst_maxi<ap_uint<32> > &dw_in_burst)
{
    // ZHR-92 (2026-08-30): single consolidated shape-range check, csim-
    // only (see mac_array.h's own header comment on
    // mac_check_supported_shape for the full rationale and the three
    // known instances it guards against). Checked once, here, at
    // run_layer's very entry -- before either op_type branch below --
    // rather than scattered per-mechanism.
    mac_check_supported_shape(d);

    // ZHR-92 Phase 1 Step 2 integration (2026-08-28): DW is now an
    // independent top-level branch, one raster pass per layer per ot,
    // BEFORE the (rt,colt) tile loop below -- exactly the
    // run_layer_raster_streaming_design_v1.md shape. Everything from
    // here down (PW's own dispatch, every shared helper) is untouched;
    // DW ops return before ever reaching it.
    if (d.op_type == LDESC_OP_DWCONV) {
        /* ZHR-92 round (2026-09-11/13): DW_OUTPUT_BURST (ON by default
         * since 2026-09-13, see dw_raster_layer.h) -- DW's packed word
         * writeout reuses
         * PW_FLAT's own EXISTING out_burst port rather than declaring a
         * 6th write port on gmem_act. DW and PW are strictly mutually
         * exclusive -- this branch early-returns before any PW code runs,
         * so the two can never issue on the same dispatch. Motivation is
         * a real, measured root cause, not tidiness: the 6th port widened
         * gmem_act's own store-unit write-request FIFO (mem_reg[5][65] vs
         * [64], direct evidence from the timing report) and deepened its
         * arbitration logic, which landed on the already-marginal path
         * feeding the shared multiplier and cost -0.25ns of WNS -- more
         * than the port's own functional win was worth. See CLAUDE.md's
         * own "adding an m_axi port to an existing bundle is not free"
         * entry. */
        run_dw_layer_raster(in_base, w_base, b_base, out_base, dw_in_burst,
#ifdef DW_OUTPUT_BURST
                             out_burst,
#endif
                             d.cin, d.cout, d.h_in, d.w_in,
                             d.k, d.stride, d.pad, d.fpg,
                             d.in_off, d.w_off, d.b_off, d.out_off,
                             d.shift_off,
                             d.in_ch_stride, d.out_ch_stride, d.h_out, d.w_out);
        return;
    }
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

    /* ZHR-92 round (2026-09-02): PW weight residency, RETRY of the
     * 2026-08-22 pw_weight_cache revert (see that comment above) -- NOT a
     * blind retry, a re-sized one. Board-measured 2026-09-02 (same round's
     * own address-fix probes): weight-read address is 55.3% of a real PW
     * layer's time (28.03ms/50.70ms, entry3), explaining ~67% of PW's
     * total/internal "external cost" gap -- the OLD 1.3% figure was
     * measured on the pre-PW_FLAT architecture (run_reduce_unified) and
     * does not apply here; do not re-cite it as a reason to skip this.
     *
     * Sizing is the actual fix, not a different partition strategy (see
     * below): the old cache was 442,368 elements (432KB), sized for the
     * single worst-case layer -- 68.6% of the whole device's 630KB BRAM on
     * its own, which is why it was ~89-95% of budget and the #2 worst P&R
     * path regardless of today's much better LUT/DSP headroom (BRAM's
     * 140-tile ceiling is a separate, fixed resource that other-resource
     * headroom does not touch -- confirmed the hard way with DSP-packing/
     * gmem_w bandwidth elsewhere in this file). First deployed round sized
     * this cache at 147,456 (144KB, layer_0040_pwconv's own weight count),
     * covering only 22 of 26 real PW layers -- see below for why that was
     * upgraded.
     *
     * UPGRADED 2026-09-02 (ZHR-92, real board evidence, same round the
     * first 144KB version deployed): the 4 layers that fell back to direct
     * DRAM read (layer 43/44/47/48, cin*cout>144KB) were board-measured
     * (PW_FIX_WADDR probe, cross-validated on two physically different P&R
     * implementations per this file's own methodology) saving 351.44ms
     * combined (78.7% of their own 446.42ms, remarkably consistent
     * 77.9-79.3% across all four) if their weight read were also cached --
     * 16.65% of the full network's own 2,111.27ms. This REFUTES an
     * intuition that had gone unchecked until measured: these 4 layers have
     * much larger cin/cout than the small cached layers, so "deeper/wider
     * layers are more compute-bound, weight-read is a smaller fraction of
     * their time" seemed plausible going in -- measured, the opposite
     * holds: their weight-read time fraction (78.7%) is HIGHER than
     * entry3's own 55.3%, not lower. Mechanism: these are late-network
     * layers with SMALL spatial extent (h=w=8, only 4 tiles) and LARGE
     * channel counts -- weight volume scales with cin*cout while compute
     * work per tile is comparatively modest at this spatial size, so
     * weight traffic dominates more, not less, than in the shallow/wide
     * layers the cache was originally sized around. Real full-network
     * P&R (BRAM 74/140=52.86% at 144KB, vs. an isolated-csynth projection
     * of 98/140=70% -- isolated was pessimistic here, not optimistic; see
     * this file's own "isolated vs real, direction not consistent" entries)
     * gave grounds to test whether 144KB's own real single-copy BRAM cost
     * (40 tiles measured vs. 32 theoretical, 1.25x overhead -- confirming
     * real synthesis does NOT duplicate this array across the 2
     * FAST_WRITEOUT instances the way isolated csynth predicted) would
     * extrapolate favorably to covering all 26 real PW layers at once.
     *
     * ATTEMPTED AND REVERTED, same day: raised PW_WEIGHT_CACHE_ELEMS to
     * 442,368 (432KB, cin*cout=384*1152, the real max across all 26 real
     * PW layers) to make every real PW layer cacheable at once. Real whole-
     * IP P&R: BRAM Block RAM Tile 140/140 (100.00%, the device's absolute
     * ceiling, zero margin) and WNS=-1.075730ns (a real timing violation,
     * not closed) -- both numbers land in the "stop, don't tune pblock,
     * switch approaches" zone this project's own hard-stop-list already
     * established for pblock-based timing rescue attempts on THIS kind of
     * signature. Isolated csynth had projected 115% BRAM (323/280
     * BRAM_18K) for this exact change, below the round's own 130%
     * pre-registered "definitely won't fit" cutoff, so real P&R was run
     * anyway per that pre-registration -- it answered definitively: this
     * specific single-cutoff-covers-everything approach does not fit.
     * Reverted PW_WEIGHT_CACHE_ELEMS back to 147,456 (144KB,
     * layer_0040_pwconv's own weight count) -- the real, P&R-verified,
     * currently-deployed configuration (WNS=+0.153200ns closed, BRAM
     * 74/140=52.86%) -- covering 22 of 26 real PW layers; layer
     * 43/44/47/48 (cin*cout>144KB) fall back to the pre-existing direct-
     * DRAM-read path, unchanged. Real board evidence (PW_FIX_WADDR probe,
     * 2026-09-02) confirms these 4 layers would save 351.44ms combined
     * (78.7% of their own 446.42ms, 16.65% of the full network) if
     * cached -- a real, substantial, NOT-YET-CAPTURED opportunity that
     * this specific attempt (raise the cutoff to cover them directly)
     * failed to realize. The pre-registered next candidate is chunked
     * loading (cutoff unchanged, the 4 big layers load their weight in
     * <=144KB pieces, reusing pw_weight_cache across chunks instead of
     * needing a bigger buffer) -- BRAM-neutral by construction (same
     * 144KB buffer), but needs its own new outer-chunk loop AND a fresh
     * re-verification of "activation read costs ~0%" (board-confirmed
     * under the CURRENT non-chunked design; chunking these 4 layers would
     * make `COPY_FROM_ROW` re-run 3x per layer, an assumption never yet
     * tested). Not attempted this round.
     *
     * PREREQUISITE PROBE DONE, 2026-09-02 (PW_FIX_ACTADDR on the same 4
     * entries, WNS=+0.129376ns, clean/no cross-validation needed): current
     * activation-read contribution measured at just 1.32ms combined /
     * 446.42ms baseline (0.30%) -- confirms entry3's "~0%" finding
     * transfers to these small-spatial/large-channel layers too, not
     * assumed. Extrapolating x3 (chunking triples COPY_FROM_ROW's re-runs)
     * adds ~2.64ms. Chunked-load overhead, using the established
     * PW_WEIGHT_HOIST cost model (~1.475ms per 144KB chunk at II=1/100MHz,
     * proportional to bytes moved): L43/L44 each need 3 chunks (442,368B =
     * exactly 3x144KB) = 4.425ms each; L47/L48 each need 2.5 chunks worth
     * of bytes (368,640B) = 3.6875ms each; total load overhead = 16.225ms
     * (currently zero -- the direct-read fallback has no separate load
     * phase). Net = 351.44 - 2.64 - 16.225 = ~332.6ms, clearing this
     * round's own pre-registered >200ms "write the chunked loop" threshold
     * by a wide margin.
     *
     * IMPLEMENTED, 2026-09-03 (ZHR-92): PW_WCHUNK (see below, wrapping the
     * whole (rt,colt) spatial sweep) -- csim 6/6 including a genuinely
     * uneven-chunk case (L47-shaped, 384+384+192). Real P&R initially
     * regressed HARD (WNS=-2.264415ns) via a NEW instance of this file's
     * own "shared multiplier bound to FSM state" mechanism -- took TWO
     * separate accumulator fixes (not one) to close, because PW_WCHUNK's
     * own new outer loop level restructured run_layer's FSM enough that
     * the SAME sink resource got hit by two DIFFERENT specific causes in
     * sequence (ot_out_ch_base's multiply first, WNS=-1.127835ns after
     * fixing it; then total_iters's multiply, exposed only once the first
     * fix changed the FSM again). See pw_flat_pipeline_impl's own header
     * comment (its `ot_out_ch_base_init`/`total_iters_in` parameters) for
     * the full two-round diagnosis, and CLAUDE.md's own refined
     * shared-multiplier entry for the general lesson. **Final real P&R:
     * WNS=+0.133715ns (closed), BRAM 74/140=52.86% (exactly unchanged from
     * the 144KB baseline), LUT +711/+1.34pp.** Not yet board-tested as of
     * this entry -- board verification (all 4 previously-fallback layers +
     * full network) is the next step before promoting to deployed baseline.
     *
     * Partitioning: deliberately NONE, unchanged from the 144KB round.
     * PW_FLAT reads exactly one weight element per pipeline step
     * (lane_w[dd], dd only ranges over MAC_PD=1) -- a plain, unpartitioned
     * BRAM array already provides one read per cycle, which is all
     * PW_FLAT ever asks for, regardless of the array's total depth. The
     * historical revert's own diff does not show a `complete`/`cyclic`
     * partition on pw_weight_cache, and 89-95% BRAM utilization for a
     * 442,368-element array is not the signature complete-partitioning
     * would leave (that would show up as a LUT explosion instead, this
     * array being far too large to register-partition) -- the historical
     * failure is attributed to sheer size against total device BRAM
     * capacity, not partition choice. */
#define PW_WEIGHT_CACHE_ELEMS 147456
    static wt_t pw_weight_cache[PW_WEIGHT_CACHE_ELEMS];

    /* ZHR-92 round (2026-09-03): chunked weight loading -- see this
     * define's own long header comment above for the round-by-round
     * history (144KB fixed cutoff -> failed 432KB single-cutoff attempt
     * -> real-board-measured 332.6ms net opportunity via chunking -> this).
     * Every PW layer now goes through pw_weight_cache; layers whose full
     * weight (cin*cout) exceeds the 144KB cache are served via MULTIPLE
     * chunks, loaded one at a time, instead of falling back to direct DRAM
     * read. Chunk boundary is per-OT (output channel), never mid-ot: each
     * ot's own cin-wide weight row is contiguous in w_base and processed
     * atomically by pw_flat_pipeline_impl (one ot fully gathered+written
     * before the next begins), so any other split granularity would need
     * to break a single ot's own gather across two loads.
     * pw_ot_per_chunk = floor(cache_size/cin) is the most ot's that fit in
     * one 144KB load; pw_n_chunks = ceil(cout/pw_ot_per_chunk). For the 22
     * real layers whose whole weight already fits under 144KB this reduces
     * to the SAME degenerate single-chunk case as before (pw_n_chunks==1
     * exactly when cin*cout<=PW_WEIGHT_CACHE_ELEMS, the OLD pw_cacheable
     * condition, by construction of floor/ceil) -- same PW_WEIGHT_HOIST
     * load shape, same pw_flat_pipeline call (ot_start=0, ot_count=
     * d.cout), bit-identical to the pre-chunking build. For the 4 real
     * layers that used to fall back to direct read (L43/44/47/48), this
     * produces 3 chunks each (verified against real descriptors: L43/44
     * exactly even 384/128 ot's per chunk; L47/48 uneven, last chunk
     * smaller -- 384+384+192 and 153+153+78 respectively, matching the
     * probe round's own byte-based load-overhead estimate exactly).
     *
     * pw_cached_ok is the renamed, narrowed old `pw_cacheable`: it now
     * only asks "does a SINGLE ot's weight (cin elements) fit in the
     * cache", not "does the whole layer fit" -- true for every real/
     * tested shape (max real cin=1152, cache=147,456 elements, nowhere
     * close). The direct-DRAM-read branch inside pw_flat_pipeline_impl
     * (pw_cached==false) is therefore DEAD CODE on any shape this project
     * can currently construct or test (would need cin>147,456, far past
     * MAX_CIN=1152) -- kept, not deleted, until this chunked mechanism is
     * board-verified, per this project's own convention for prior
     * dead-but-kept fallbacks (use_wide_path, PW_PATCH_HOIST's
     * in_base_wide parameter). */
    bool pw_cached_ok = (d.op_type == LDESC_OP_PWCONV) && (d.cin <= PW_WEIGHT_CACHE_ELEMS);
    int pw_ot_per_chunk = 1;
    int pw_n_chunks = 1;
    if (pw_cached_ok) {
        pw_ot_per_chunk = PW_WEIGHT_CACHE_ELEMS / d.cin;
        if (pw_ot_per_chunk < 1) pw_ot_per_chunk = 1;
        pw_n_chunks = (d.cout + pw_ot_per_chunk - 1) / pw_ot_per_chunk;
    }
    /* ZHR-92 round (2026-09-03, SECOND shared-multiplier fix, same round as
     * the ot_out_ch_base one below): layer-constant (division only, not a
     * multiply, and independent of which chunk is being processed) --
     * mirrors pw_flat_pipeline_impl's own local n_cbase exactly, computed
     * here too so pw_total_iters (below, per-chunk) doesn't need to. */
    int pw_n_cbase = (d.cin + MAX_CIN_PW - 1) / MAX_CIN_PW;

    /* ZHR-92 round (2026-09-03, shared-multiplier regression fix): both
     * pw_w_chunk_off (PW_WEIGHT_HOIST's own DRAM read address, below) and
     * pw_ot_out_ch_base (threaded into pw_flat_pipeline/_narrow as
     * ot_out_ch_base_init, replacing that function's own former internal
     * `ot_start * d.out_ch_stride` multiply) are genuine loop-carried
     * accumulators now, not `pw_ot_lo * stride`-shaped multiplies -- see
     * pw_flat_pipeline_impl's own header comment for the full real-P&R
     * diagnosis (WNS -2.264415ns, traced to exactly this class of multiply
     * getting bound into the shared mul_32s_32s_32_2_1 resource). Stepped
     * by THIS chunk's own REAL (possibly clamped) pw_ot_count, not the
     * nominal pw_ot_per_chunk -- deliberately robust to an uneven last
     * chunk (L47/L48-shaped: 384+384+192) even though the accumulator's
     * post-increment value is provably never consumed after the true last
     * chunk either way; stepping by the real count needs no argument for
     * why it's safe, unlike a fixed-step version would. */
    int pw_w_chunk_off = 0;
    int pw_ot_out_ch_base = 0;
    /* SHARED_MUL_ARMS round (2026-09-12): wchunk*pw_ot_per_chunk was arm #6
     * of the shared multiplier; now a per-chunk accumulator. */
    int pw_ot_lo = 0;
    /* iters_per_ot: PW_FLAT_STEPS_PER_CBASE and PW_FLAT_WRITEOUT_ELEMS are
     * compile-time constants (8 and 16), so this is a shift+add, no
     * multiplier. */
    const int pw_iters_per_ot = pw_n_cbase * PW_FLAT_STEPS_PER_CBASE + PW_FLAT_WRITEOUT_ELEMS;

    PW_WCHUNK: for (int wchunk = 0; wchunk < pw_n_chunks; wchunk++) {
    int pw_ot_count = pw_ot_per_chunk;
    if (pw_cached_ok) {
        int remain = d.cout - pw_ot_lo;
        if (pw_ot_count > remain) pw_ot_count = remain;
    }
    /* SHARED_MUL_ARMS round (2026-09-12): the four per-chunk products of
     * pw_ot_count -- w_total = cin*pw_ot_count (PW_WEIGHT_HOIST bound, AND
     * the pw_w_chunk_off step, the same product HLS bound twice), the
     * pw_ot_out_ch_base step (pw_ot_count*out_ch_stride) and pw_total_iters
     * (pw_ot_count*iters_per_ot) -- were four of the six run_layer arms on
     * the shared 32x32 multiplier (FSM states 88/101/103, plus the local
     * U1439). FIRST ATTEMPT this round: one add-loop accumulating all
     * three sums over pw_ot_count iterations -- HLS's loop-idiom pass
     * recognised "add a loop-invariant N times" and rewrote it BACK into
     * three i32 multiplies on the same shared unit (binding DB:
     * mul_ln1056/_1/_2, Predicate icmp_ln1056) -- an add-loop is not a
     * way to avoid a multiplier here. What works (same technique as the
     * top-level scalar_hw/scalar_total): NARROW operand types, so HLS
     * emits distinct small multiplier cores (mul_11ns_11ns_22 etc.) that
     * cannot be bound onto mul_32s_32s_32 at all -- no arm, no mux.
     * Ranges (real network): pw_ot_count <= cout <= 1152 (11 bits), cin <=
     * 1152 (11), out_ch_stride = h_out*w_out <= 16384 (15), iters_per_ot =
     * n_cbase*8+16 <= 304 (10); products 22/26/21 bits. Asserted in csim
     * (silent truncation otherwise). Verified in the binding DB, not
     * assumed. */
#ifndef __SYNTHESIS__
    assert(pw_ot_count >= 0 && pw_ot_count < (1 << 11) && d.cin < (1 << 11) && d.out_ch_stride >= 0 && d.out_ch_stride < (1 << 15) && pw_iters_per_ot < (1 << 10));
#endif
    const ap_uint<11> pw_cnt_n    = pw_ot_count;
    const ap_uint<11> pw_cin_n    = d.cin;
    const ap_uint<15> pw_stride_n = d.out_ch_stride;
    const ap_uint<10> pw_ipo_n    = pw_iters_per_ot;
    const ap_uint<22> w_total_n          = pw_cnt_n * pw_cin_n;
    const ap_uint<26> pw_chunk_ch_span_n = pw_cnt_n * pw_stride_n;
    const ap_uint<21> pw_total_iters_n   = pw_cnt_n * pw_ipo_n;
    const int w_total          = (int)w_total_n;
    const int pw_chunk_ch_span = (int)pw_chunk_ch_span_n;
    const int pw_total_iters   = (int)pw_total_iters_n;
    if (pw_cached_ok) {
        PW_WEIGHT_HOIST: for (int i = 0; i < w_total; i++) {
            #pragma HLS PIPELINE II=1
            pw_weight_cache[i] = w_base[d.w_off + pw_w_chunk_off + i];
        }
    }
    /* ZHR-92 round (2026-09-03, SECOND shared-multiplier fix): computed
     * ONCE per chunk here (loop-invariant across the whole (rt,colt)
     * spatial sweep below) instead of once per (rt,colt) tile inside
     * pw_flat_pipeline_impl -- see that function's own header comment for
     * the full real-P&R diagnosis (critical path moved to run_layer's own
     * FSM after the first fix, still sinking into the shared multiplier,
     * this time via total_iters's multiply). Passed in as total_iters_in. */
    /* pw_total_iters: now a narrow-typed product above (SHARED_MUL_ARMS
     * round, 2026-09-12) -- same value, not on the shared 32x32 unit. */

    /* SHARED_MUL_ARMS round (2026-09-12): rt*MAC_PR*W (ROW_READ input rows)
     * and rt*MAC_PR*w_out (PW writeout rows) as rt-loop accumulators --
     * stepped by MAC_PR*W / MAC_PR*w_out (MAC_PR is a compile-time 4, a
     * shift), so neither ever touches a multiplier. Re-initialised per
     * PW_WCHUNK chunk because the rt loop restarts per chunk. */
    int pw_rt_in_base  = 0;
    int pw_rt_row_base = 0;
    for (int rt = 0; rt < d.n_row_tiles; rt++) {
        int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
        int rt_row_base = pw_rt_row_base;   /* == rt*MAC_PR*w_out for THIS rt */
        int rr_w = 0;                       /* == rr*W inside ROW_READ, stepped there */
        int rt_in_base  = pw_rt_in_base;    /* == rt*MAC_PR*W for THIS rt */
        pw_rt_row_base += MAC_PR * d.w_out; /* for the NEXT rt */
        pw_rt_in_base  += MAC_PR * W;

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
#ifdef PW_ROWREAD_MERGE4
            /* ZHR-92 (2026-09-15) PW_ROWREAD_MERGE4 -- ON BY DEFAULT since
             * 2026-09-15 (define after the includes; PW_ROWREAD_MERGE4_OFF
             * reverts), deployed as mac_array_a3_merge4. History below as
             * written during the round. Originally a STEP-1 MECHANISM PROBE,
             * OFF by default: (a)+(b) together.
             * After PW_ROWREAD_PREFETCH the real-board fit still had 34 cycles
             * per ROW_READ request (61ms), UNIFORM across layers and independent
             * of cin -- i.e. not AXI latency any more but loop overhead: the
             * FILL loop ran its compile-time MAX_WORDS_PER_CH = 17 iterations
             * regardless of n_words (2-16), plus ~17 cycles of fill/drain,
             * request issue and loop control per request.
             * (b) ONE request per (rt, ci) covering the row tile's 4 input rows:
             * contiguity verified first (all 26 PW layers have in_ch_stride ==
             * h_in*w_in, i.e. packed rows; ROW_READ reads WHOLE rows, k=1/s=1;
             * the tile's rows are oh = rt*4 + rr, consecutive) -- so the 4 rows
             * are one contiguous 4*W-byte span, unlike a DW output tile whose
             * 4 rows are w_out apart. Requests: 179,904 -> 44,976.
             * (a) the FILL loop's bound is the RUNTIME word count of the span,
             * 4*W/4 = W words (+1 if unaligned), not a compile-time constant --
             * (b) needs (a): a fixed bound would be 65 and worse than today for
             * small W. This is a pipelined loop's trip count, NOT a runtime
             * value feeding an unrolled index (the DW_PATCH_STAGE II-blowup
             * shape) -- confirmed in the schedule, not assumed.
             * The 4 byte lanes now cross row boundaries inside the span: a
             * loop-carried (cur_row, cur_col) counter walks the span, lane b's
             * (row, col) chains from lane b-1 with a wrap at W. row_buf is
             * complete dim=1 (4 row arrays) + cyclic-4 dim=2: 4 consecutive
             * columns of one row hit 4 distinct banks, and at a row boundary
             * the lanes split across 2 row ARRAYS -- no two lanes ever hit the
             * same array+bank (W is a multiple of 4 on every real layer;
             * for W=1/2 the lanes land on different row arrays). II=1 is
             * structurally possible; whether HLS proves it is what this step
             * measures.
             * Partial last row tile (only the 2 SE fc layers, h_in=w_in=1,
             * last_row_tile=1): the old code clamped rows rr >= r_sz to row 0
             * and COPY_FROM_ROW discards them via r_valid; the merged span
             * reads 4 bytes = the next 3 channels' single bytes into rows
             * 1..3, which the SAME r_valid discards -- same result, different
             * path (and up to 3 bytes past the input tensor for the last
             * channel, inside the arena). Real conv PW layers all have
             * h_in % 4 == 0, no overrun.
             * Prefetch depth scales with the span so the in-flight words stay
             * <= ~136 of the adapter's 256-word buffer and <= 16 outstanding
             * bursts (MAX_READ_BURST_LENGTH 16): W>32 -> 2, W>16 -> 4, W>8 -> 6,
             * else 8. Runtime guard in the prime loop, not in the hot loop. */
            /* Merged path only when every word of the span lies inside one row
             * and starts word-aligned: W % 4 == 0 (then in_ch_stride = h*W and
             * rt_in_base = rt*4*W are multiples of 4 too) and in_off % 4 == 0.
             * True for every real conv PW layer (W in {8,16,32,64}); the W=1
             * SE fc layers and any synthetic W<4 / unaligned shape take the
             * original per-(rr,ci) path below. A per-LAYER branch around two
             * loop nests, not a gate inside a pipeline. */
            const bool row_wide_ok = ((W & 3) == 0) && ((d.in_off & 3) == 0);
            if (row_wide_ok) {
                const int span_bytes = MAC_PR * W;                 /* 4 rows, contiguous */
                const int pf_dyn = (W > 32) ? 2 : (W > 16) ? 4 : (W > 8) ? 6 : PW_ROWREAD_PF;
                int ch_base = 0;       /* == ci * d.in_ch_stride, accumulated */
                int flat_base = 0;     /* == ci * W, accumulated -- row_buf's own flat layout */
                int ch_base_req = 0;   /* == ci_req * d.in_ch_stride, accumulated */
                ROW_READ_PRIME4: for (int pf = 0; pf < PW_ROWREAD_PF; pf++) {
                    if (pf < Cin && pf < pf_dyn) {
                        int rq_addr = d.in_off + ch_base_req + rt_in_base;
                        int rq_words = ((rq_addr & 3) + span_bytes + 3) >> 2;
                        in_burst.read_request((size_t)(rq_addr >> 2), (unsigned)rq_words);
                        ch_base_req += d.in_ch_stride;
                    }
                }
                ROW_READ_CH4: for (int ci = 0; ci < Cin; ci++) {
                    const int n_words = span_bytes >> 2;            /* runtime trip count; aligned, no partial word */
                    /* Per-WORD (row, col) state, not per-lane chaining: the first
                     * formulation chained lane b's (row, col) from lane b-1 and
                     * HLS could not prove the 4 stores hit distinct banks --
                     * store-vs-store 200-880, II=4 (csynth, 2026-09-15). Here
                     * lane b's column is cur_col + b (+ W to the next row on a
                     * spill), i.e. the same `base + lane` address form the old
                     * per-row FILL had, which HLS proved bank-distinct. cur_col
                     * is the span position of this word's first byte within its
                     * row; a word can straddle at most one row boundary when
                     * W >= 4 (every real layer). For the W=1/2 SE fc layers the
                     * rows > 0 are discarded downstream anyway (see above). */
                    /* Third formulation (the second, per-word with a one-row
                     * spill, still got II=4 -- HLS cannot learn that a loop-
                     * carried column with a wrap-at-W stays 0 mod 4): the bank
                     * is made EXPLICIT. row_buf is cyclic-4 on dim 2, so bank ==
                     * index & 3; the index is built as (word << 2) | b, whose
                     * low two bits ARE the lane -- provably distinct banks -- and
                     * with W % 4 == 0 and an aligned span a word never straddles
                     * a row, so all 4 lanes write the SAME row array. */
                    const int W_words = W >> 2;
                    int widx = flat_base >> 2;                      /* word index of this word within its row array */
                    int col_w = 0;                                  /* word column within the row */
                    int cur_row = 0;
                    ROW_READ_FILL4: for (int i = 0; i < n_words; i++) {
                        #pragma HLS PIPELINE II=1
                        ap_uint<32> wd = in_burst.read();
                        for (int b = 0; b < 4; b++) {
                            #pragma HLS UNROLL
                            row_buf[cur_row][(widx << 2) | b] = (act_t)wd.range(b * 8 + 7, b * 8);
                        }
                        widx++;
                        col_w++;
                        if (col_w == W_words) { col_w = 0; cur_row++; widx = flat_base >> 2; }
                    }
                    if (ci + pf_dyn < Cin) {
                        int rq_addr = d.in_off + ch_base_req + rt_in_base;
                        int rq_words = ((rq_addr & 3) + span_bytes + 3) >> 2;
                        in_burst.read_request((size_t)(rq_addr >> 2), (unsigned)rq_words);
                        ch_base_req += d.in_ch_stride;
                    }
                    ch_base += d.in_ch_stride;
                    flat_base += W;
                }
            } else {
#endif
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
                /* SHARED_MUL_ARMS round (2026-09-12): oh*W was one of the 6
                 * run_layer arms on the shared 32x32 multiplier (FSM state
                 * 105). oh_w == (rt*MAC_PR + (r_valid ? rr : 0)) * W, built
                 * from pw_rt_in_base (rt loop accumulator, += MAC_PR*W per
                 * rt) and rr_w (+= W per rr). Zero multiplies. */
                int oh_w = rt_in_base + (r_valid ? rr_w : 0);
                rr_w += W;
                int ch_base = 0;    /* == ci * d.in_ch_stride, accumulated -- no runtime multiply */
                int flat_base = 0;  /* == ci * W, accumulated -- row_buf's own flat layout */
#ifdef PW_ROWREAD_PREFETCH
                /* ZHR-92 (2026-09-15) PW_ROWREAD_PREFETCH -- ON BY DEFAULT since
                 * 2026-09-15 (define after the includes; PW_ROWREAD_PREFETCH_OFF
                 * reverts), deployed as mac_array_a3_pwpf. History below as
                 * written during the round. Originally a STEP-1 MECHANISM PROBE,
                 * OFF by default. Real-board fit after PW_DEFER_WRESP: 71-85
                 * cycles per ROW_READ request for only w/4 (2-16) words --
                 * 179,904 requests = 128-153ms, ~45% of PW -- i.e. each
                 * request's own latency is paid serially: request, wait,
                 * read n_words, next. This variant issues the requests
                 * PW_ROWREAD_PF channels AHEAD: the first PF requests are
                 * primed before ROW_READ_CH, and after each channel's fill
                 * the request for ci+PF is issued, so up to PF request
                 * latencies overlap with the fills. The read() stays exactly
                 * where it is, inside ROW_READ_FILL at II=1.
                 * NOT the same thing as the 2026-08-29 ROW_READ DATAFLOW
                 * split: that overlapped produce with consume when the read
                 * side was ~4% of PW ("total ~ max" held but was worth 4%);
                 * here the request LATENCY is 42% of PW (128/307ms) -- same
                 * shape of idea, completely different magnitude; do not
                 * dismiss this on that round's result.
                 * PF sizing from the exported gmem_act adapter, not defaults:
                 * NUM_READ_OUTSTANDING = 16, request FIFO USER_MAXREQS = 16,
                 * load-unit read-data buffer RBUFF_DEPTH = 16*16 = 256 words
                 * (the adapter issues an AR only with buffer credit, so a
                 * full buffer stalls AR issue, never deadlocks -- provided
                 * our own outstanding count stays <= 16). Real PW rows are
                 * <= 16 words (w <= 64): PF = 8 -> <= 8 in flight, <= 128
                 * words buffered, prefetch distance >= 8*(n_words+~5) >= 56
                 * cycles even for the w=8 layers. Addresses are the SAME
                 * accumulator arithmetic as the fill side (ch_base_req +=
                 * in_ch_stride), so n_words recomputed at fill time from
                 * ch_base matches the request's -- no queue, no multiply
                 * (checked in the binding DB, not assumed). */
                int ch_base_req = 0;   /* == ci_req * d.in_ch_stride, accumulated */
                ROW_READ_PRIME: for (int pf = 0; pf < PW_ROWREAD_PF; pf++) {
                    if (pf < Cin) {
                        int rq_addr = d.in_off + ch_base_req + oh_w;
                        int rq_words = ((rq_addr & 3) + W + 3) >> 2;
                        in_burst.read_request((size_t)(rq_addr >> 2), (unsigned)rq_words);
                        ch_base_req += d.in_ch_stride;
                    }
                }
#endif
                ROW_READ_CH: for (int ci = 0; ci < Cin; ci++) {
                    int byte_addr = d.in_off + ch_base + oh_w;
                    int word_addr0 = byte_addr >> 2;
                    int r = byte_addr & 3;
                    int n_words = (r + W + 3) >> 2;
                    /* ZHR-92 round (2026-09-04): PW_FIX_ROWREAD_ADDR --
                     * timing-only probe, mirrors PW_FIX_WADDR's own
                     * discipline (see its comment elsewhere in this file)
                     * but for ROW_READ's own real DRAM burst fetch, which
                     * PW_FIX_ACTADDR never touched (that flag only fixes
                     * COPY_FROM_ROW's SRAM-to-SRAM copy further down, not
                     * this read). ONLY the address argument is fixed
                     * (d.in_off, always in-range, same word_addr0 for
                     * every (rr,ci)) -- n_words is left computed from the
                     * REAL r/W exactly as before, so burst LENGTH is
                     * unchanged from the real run; only address locality
                     * changes. Values WILL be wrong. Board-only, csim/
                     * checkpoint NOT meaningful under this flag. Off by
                     * default. */
#ifdef PW_ROWREAD_PREFETCH
                    (void)word_addr0;      /* this channel's request was issued PF channels ago */
#elif defined(PW_FIX_ROWREAD_ADDR)
                    in_burst.read_request((size_t)(d.in_off >> 2), (unsigned)n_words);
#else
                    in_burst.read_request((size_t)word_addr0, (unsigned)n_words);
#endif
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
#ifdef PW_ROWREAD_PREFETCH
                    if (ci + PW_ROWREAD_PF < Cin) {
                        int rq_addr = d.in_off + ch_base_req + oh_w;
                        int rq_words = ((rq_addr & 3) + W + 3) >> 2;
                        in_burst.read_request((size_t)(rq_addr >> 2), (unsigned)rq_words);
                        ch_base_req += d.in_ch_stride;
                    }
#endif
                    ch_base += d.in_ch_stride;
                    flat_base += W;
                }
            }
#ifdef PW_ROWREAD_MERGE4
            } /* !row_wide_ok */
#endif
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
                            /* ZHR-92 round (2026-09-02): PW_FIX_ACTADDR --
                             * timing-only probe, mirrors PW_FIX_WADDR exactly
                             * (see its comment above). row_buf[rr][0] is
                             * always in-range (rr itself is untouched/still
                             * real). Off by default. */
#ifdef PW_FIX_ACTADDR
                            pw_patch_full[ci][rr][cw] = valid
                                ? row_buf[rr][0]
                                : (act_t)0;
#else
                            pw_patch_full[ci][rr][cw] = valid
                                ? row_buf[rr][flat_base + colt * MAC_PC + cw]
                                : (act_t)0;
#endif
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
                /* pw_narrow (FAST_WRITEOUT's own gate, template dispatch,
                 * unchanged/untouched by this round) and pw_cached_ok
                 * (PW_WEIGHT_HOIST's own gate, a runtime bool passed INTO
                 * whichever pw_narrow instance gets picked) are two
                 * INDEPENDENT conditions on two INDEPENDENT dimensions --
                 * do not conflate them. ZHR-92 round (2026-09-03): the old
                 * >144KB fallback (this comment used to describe) is GONE
                 * as a real dispatch path -- pw_cached_ok is now true for
                 * every real/tested layer (see PW_WEIGHT_CACHE_ELEMS's own
                 * header comment above run_layer), and >144KB layers are
                 * served via the PW_WCHUNK loop (above) calling this same
                 * dispatch once per chunk with a restricted (ot_start,
                 * ot_count) instead of falling back to a different
                 * mechanism entirely. */
                bool pw_narrow = (d.last_col_tile < MAC_PC) || ((d.out_off & 3) != 0);
                if (pw_narrow) {
                    pw_flat_pipeline_narrow(d, w_base, pw_weight_cache, pw_cached_ok, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt_row_base, colt, r_sz, col_sz, pw_ot_lo, pw_ot_count, pw_ot_out_ch_base, pw_total_iters);
                } else {
                    pw_flat_pipeline(d, w_base, pw_weight_cache, pw_cached_ok, pw_patch_full, pw_bias_cache, pw_shift_cache, out_base, out_burst, rt_row_base, colt, r_sz, col_sz, pw_ot_lo, pw_ot_count, pw_ot_out_ch_base, pw_total_iters);
                }
            }
        }
    }
    /* Accumulate for the NEXT chunk, using THIS chunk's own real (possibly
     * clamped) pw_ot_count -- see the accumulator declarations above this
     * loop for the full rationale. Both multiplies below execute once per
     * CHUNK (at most 3x per real layer), not once per (rt,colt) spatial
     * tile the way the fixed regression did -- a real, large reduction in
     * call-site frequency even though a multiply is still here. */
    pw_w_chunk_off    += w_total;            /* == pw_ot_count * d.cin (narrow product above) */
    pw_ot_out_ch_base += pw_chunk_ch_span;   /* == pw_ot_count * d.out_ch_stride (narrow product above) */
    pw_ot_lo          += pw_ot_per_chunk;    /* == (wchunk+1) * pw_ot_per_chunk */
    } // end PW_WCHUNK
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
/* ZHR-92 round (2026-09-06): ELEMWISE_BURST rewrite. Old version used
 * plain-pointer in_base[]/out_base[] (one un-bursted AXI transaction per
 * element, confirmed real-board 16.57x a naive II=1 floor -- see this
 * round's own writeout). Chunked hls::burst_maxi<ap_uint<32>> read/write
 * instead, 4 bytes/cycle (word-packed, matching the bundle's own 32-bit
 * width -- an 8-bit burst_maxi port crashed csynth's codegen when sharing
 * this bundle, see ELEMWISE_CHUNK's own header comment in mac_array.h):
 * in_off's chunk is read into a small on-chip word buffer first (buf_a),
 * then in2_off's matching chunk is read and summed on the fly (4 lanes
 * unrolled per word) while writing -- two SEQUENTIAL passes over the SAME
 * elemwise_in_burst port, not two simultaneous reads, which is what
 * caused the old version's confirmed `Unable to schedule bus request
 * operation... due to limited memory ports` II=2 violation (two same-
 * cycle reads on one bundle). Whether this incidentally restores II=1 is
 * measured via csynth, not assumed here. Word-packing is safe with zero
 * tail handling: confirmed via real descriptors (all 27 real GELU/ADD
 * entries) that in_off/in2_off/out_off and total (=cin*h*w) are ALL
 * exactly mod4==0. */
static void run_add(const LayerDescV2 &d, int total,
                     hls::burst_maxi<ap_uint<32> > in_burst, hls::burst_maxi<ap_uint<32> > out_burst)
{
    const int n_words   = total >> 2;
    const int in_off_w  = d.in_off  >> 2;
    const int in2_off_w = d.in2_off >> 2;
    const int out_off_w = d.out_off >> 2;
    ap_uint<32> buf_a[ELEMWISE_CHUNK_WORDS];
    ADD_CHUNK: for (int base = 0; base < n_words; base += ELEMWISE_CHUNK_WORDS) {
        int this_chunk = (n_words - base < ELEMWISE_CHUNK_WORDS) ? (n_words - base) : ELEMWISE_CHUNK_WORDS;
        in_burst.read_request(in_off_w + base, this_chunk);
        ADD_READA: for (int i = 0; i < ELEMWISE_CHUNK_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            if (i < this_chunk) {
                buf_a[i] = in_burst.read();
            }
        }
        in_burst.read_request(in2_off_w + base, this_chunk);
        out_burst.write_request(out_off_w + base, this_chunk);
        ADD_COMPUTE: for (int i = 0; i < ELEMWISE_CHUNK_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            if (i < this_chunk) {
                ap_uint<32> wa = buf_a[i];
                ap_uint<32> wb = in_burst.read();
                ap_uint<32> wo = 0;
                for (int k = 0; k < 4; k++) {
                    #pragma HLS UNROLL
                    act_t a = (act_t)wa.range(k * 8 + 7, k * 8);
                    act_t b = (act_t)wb.range(k * 8 + 7, k * 8);
                    acc_t sum = (acc_t)a + (acc_t)b;
                    wo.range(k * 8 + 7, k * 8) = (ap_uint<8>)(act_t)clip_shift(sum, d.out_shift);
                }
                out_burst.write(wo);
            }
        }
        out_burst.write_response();
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
#ifdef SE_BURST
/* ZHR-92 (2026-09-15) SE_BURST -- STEP-1/csim probe, OFF by default. The
 * busy-poll harness (same day) put the SE block's GAP at 7.63 and SCALE at
 * 11.08 cycles/element (3.75 / 5.45ms): plain-pointer, one-byte-per-
 * iteration loops -- the exact shape run_gelu had at 8.6x before
 * ELEMWISE_BURST. Same mechanism here, over the same elemwise_in/out_burst
 * ports (gmem_act; GAP/SCALE never run concurrently with GELU/ADD).
 * GAP is a REDUCTION: read side only (whole plane, chunked like GELU, a
 * loop-carried channel counter -- no division inside the pipeline: the
 * per-channel sum/HW stays in a separate sequential loop, as today), the
 * one-byte-per-channel output keeps the plain out_base[] write (768 writes
 * for the real layer; no alignment question on the output). SCALE is
 * elementwise: gate vector burst-loaded once (cin bytes at in2_off,
 * word-aligned), then run_gelu's own chunked read/compute/write. Alignment
 * verified on the real descriptors before writing this (entries 75/80:
 * in_off, in2_off, out_off all mod4==0, HW=64, cin=768). */
static void run_gap(const LayerDescV2 &d, int total, int HW,
                    hls::burst_maxi<ap_uint<32> > in_burst, act_t out_base[])
{
#ifndef __SYNTHESIS__
    assert(d.cin <= ELEMWISE_MAX_CH && (HW % 4) == 0 && (d.in_off % 4) == 0 &&
           "SE_BURST run_gap: cin > ELEMWISE_MAX_CH or unaligned HW/in_off");
#endif
    const int n_words  = total >> 2;
    const int wpc      = HW >> 2;          /* words per channel (real: 16) */
    const int in_off_w = d.in_off >> 2;
    acc_t ch_sum[ELEMWISE_MAX_CH];
    int   c = 0, cnt = 0;
    acc_t sum = 0;
    GAP_CHUNK: for (int base = 0; base < n_words; base += ELEMWISE_CHUNK_WORDS) {
        int this_chunk = (n_words - base < ELEMWISE_CHUNK_WORDS) ? (n_words - base) : ELEMWISE_CHUNK_WORDS;
        in_burst.read_request(in_off_w + base, this_chunk);
        GAP_RD: for (int i = 0; i < ELEMWISE_CHUNK_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            if (i < this_chunk) {
                ap_uint<32> wi = in_burst.read();
                acc_t s4 = 0;
                for (int k = 0; k < 4; k++) {
                    #pragma HLS UNROLL
                    s4 += (acc_t)(act_t)wi.range(k * 8 + 7, k * 8);
                }
                acc_t nsum = sum + s4;
                int ncnt = cnt + 1;
                if (ncnt == wpc) {
                    ch_sum[c] = nsum;
                    sum = 0; cnt = 0; c++;
                } else {
                    sum = nsum; cnt = ncnt;
                }
            }
        }
    }
    /* PIPELINE off: HLS auto-pipelined this loop in the first csynth and
     * replaced the sequential sdiv (216 LUT) with a pipelined one (2,512
     * LUT, latency 47). 768 sequential divisions cost ~0.25ms -- keep the
     * small divider, as the pre-SE_BURST GAP_C loop had. */
    GAP_OUT: for (int cc = 0; cc < d.cin; cc++) {
        #pragma HLS PIPELINE off
        acc_t avg = ch_sum[cc] / HW;
        out_base[d.out_off + cc] = (act_t)clip_shift(avg, d.out_shift);
    }
}
#else
static void run_gap(const LayerDescV2 &d, int HW, const act_t in_base[], act_t out_base[])
{
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
#endif /* SE_BURST */

static void run_relu(const LayerDescV2 &d, int total, const act_t in_base[], act_t out_base[])
{
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

static void run_sigmoid(const LayerDescV2 &d, int total, const act_t in_base[], act_t out_base[])
{
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
/* ZHR-92 round (2026-09-06): ELEMWISE_BURST rewrite -- see run_add's own
 * header comment for the shared rationale (real-board 8.60x a naive II=1
 * floor, confirmed per-element not fixed overhead, plain-pointer access
 * never bursted) and for why the burst port is ap_uint<32>-word-packed
 * rather than act_t-typed (an 8-bit burst_maxi port sharing this bundle
 * crashed csynth's codegen). GELU's own access pattern (one read, one
 * write per element, fully sequential offsets) is the simplest possible
 * burst shape -- chunked read+compute+write in one pass, no intermediate
 * buffer needed (unlike run_add's two-source case), 4 lanes unrolled per
 * word. */
static void run_gelu(const LayerDescV2 &d, int total, hls::burst_maxi<ap_uint<32> > in_burst, hls::burst_maxi<ap_uint<32> > out_burst)
{
    const int n_words  = total >> 2;
    const int in_off_w  = d.in_off  >> 2;
    const int out_off_w = d.out_off >> 2;
    GELU_CHUNK: for (int base = 0; base < n_words; base += ELEMWISE_CHUNK_WORDS) {
        int this_chunk = (n_words - base < ELEMWISE_CHUNK_WORDS) ? (n_words - base) : ELEMWISE_CHUNK_WORDS;
        in_burst.read_request(in_off_w + base, this_chunk);
        out_burst.write_request(out_off_w + base, this_chunk);
        GELU: for (int i = 0; i < ELEMWISE_CHUNK_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            if (i < this_chunk) {
                ap_uint<32> wi = in_burst.read();
                ap_uint<32> wo = 0;
                for (int k = 0; k < 4; k++) {
                    #pragma HLS UNROLL
                    act_t x = (act_t)wi.range(k * 8 + 7, k * 8);
                    acc_t prod = (acc_t)x * (acc_t)quantized_sigmoid(x);
                    wo.range(k * 8 + 7, k * 8) = (ap_uint<8>)(act_t)clip_shift(prod, d.out_shift);
                }
                out_burst.write(wo);
            }
        }
        out_burst.write_response();
    }
}

/* SE's final gate multiply: op0 (in_off) is the full HxWxC feature map,
 * op1 (in2_off) is the C-length gate, broadcast over spatial -- confirmed
 * from tools/layer_dag_ground_truth.json: final_conv's node has fan_out=2,
 * feeding both ReduceMean AND this Mul directly from the same tensor. */
#ifdef SE_BURST
static void run_scale(const LayerDescV2 &d, int total, int HW,
                      hls::burst_maxi<ap_uint<32> > in_burst, hls::burst_maxi<ap_uint<32> > out_burst)
{
#ifndef __SYNTHESIS__
    assert(d.cin <= ELEMWISE_MAX_CH && (d.cin % 4) == 0 && (HW % 4) == 0 &&
           (d.in_off % 4) == 0 && (d.in2_off % 4) == 0 && (d.out_off % 4) == 0 &&
           "SE_BURST run_scale: cin > ELEMWISE_MAX_CH, cin%4, or unaligned HW/in_off/in2_off/out_off");
#endif
    const int n_words   = total >> 2;
    const int wpc       = HW >> 2;
    const int in_off_w  = d.in_off  >> 2;
    const int out_off_w = d.out_off >> 2;
    act_t gate_buf[ELEMWISE_MAX_CH];
    #pragma HLS ARRAY_PARTITION variable=gate_buf cyclic factor=4 dim=1
    /* gate vector: cin bytes at in2_off, one burst */
    const int n_gate_w = d.cin >> 2;
    in_burst.read_request(d.in2_off >> 2, n_gate_w);
    SCALE_GATE: for (int i = 0; i < ELEMWISE_MAX_CH / 4; i++) {
        #pragma HLS PIPELINE II=1
        if (i < n_gate_w) {
            ap_uint<32> wg = in_burst.read();
            for (int k = 0; k < 4; k++) {
                #pragma HLS UNROLL
                gate_buf[i * 4 + k] = (act_t)wg.range(k * 8 + 7, k * 8);
            }
        }
    }
    int c = 0, cnt = 0;
    SCALE_CHUNK: for (int base = 0; base < n_words; base += ELEMWISE_CHUNK_WORDS) {
        int this_chunk = (n_words - base < ELEMWISE_CHUNK_WORDS) ? (n_words - base) : ELEMWISE_CHUNK_WORDS;
        in_burst.read_request(in_off_w + base, this_chunk);
        out_burst.write_request(out_off_w + base, this_chunk);
        SCALE_W: for (int i = 0; i < ELEMWISE_CHUNK_WORDS; i++) {
            #pragma HLS PIPELINE II=1
            if (i < this_chunk) {
                ap_uint<32> wi = in_burst.read();
                act_t gate = gate_buf[c];
                ap_uint<32> wo = 0;
                for (int k = 0; k < 4; k++) {
                    #pragma HLS UNROLL
                    act_t x = (act_t)wi.range(k * 8 + 7, k * 8);
                    acc_t prod = (acc_t)x * (acc_t)gate;
                    wo.range(k * 8 + 7, k * 8) = (ap_uint<8>)(act_t)clip_shift(prod, d.out_shift);
                }
                out_burst.write(wo);
                int ncnt = cnt + 1;
                if (ncnt == wpc) { cnt = 0; c++; } else { cnt = ncnt; }
            }
        }
        out_burst.write_response();
    }
}
#else
static void run_scale(const LayerDescV2 &d, int HW, const act_t in_base[], act_t out_base[])
{
    SCALE_C: for (int c = 0; c < d.cin; c++) {
        act_t gate = in_base[d.in2_off + c];
        SCALE_HW: for (int i = 0; i < HW; i++) {
            #pragma HLS PIPELINE II=1
            acc_t prod = (acc_t)in_base[d.in_off + c * HW + i] * (acc_t)gate;
            out_base[d.out_off + c * HW + i] = (act_t)clip_shift(prod, d.out_shift);
        }
    }
}
#endif /* SE_BURST */

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
/* ZHR-92 (2026-08-29): desc/out_written moved OFF gmem_meta (m_axi) onto
 * s_axilite, eliminating the gmem_meta master entirely. Confirmed safe by
 * source read + a real board dispatch pattern (n_layers is ALWAYS 1 on
 * every real call, mac_array_full_network_test.c / mac_array_single_op_
 * test.c) -- desc[0] was the only entry ever consumed, so the n_layers
 * loop and desc[] indexing were dead generality, not live architecture.
 * Register mapping verified empirically with a standalone probe
 * (probe_desc_axilite.cpp/.tcl) before this change, not assumed: a 28-int
 * struct-by-value over s_axilite flattens into 28 consecutive 32-bit
 * registers (one per field, declaration order, no padding), and a plain
 * scalar output pointer over s_axilite (no m_axi) becomes a normal
 * Read-only data register + a standard ap_vld handshake bit -- the same
 * "value stable at ap_done" guarantee this function's own `return` value
 * already relies on. See CLAUDE.md's gmem_meta-elimination entries and
 * ZHR-92/ZHR-63 for the full recon. */
/* ZHR-92 round (2026-09-06): ELEMWISE_BURST -- gelu_burst/add_burst give
 * run_gelu/run_add their own hls::burst_maxi<act_t> access onto the SAME
 * physical gmem_act master (bundle=gmem_act, no new AXI master/BD change
 * needed -- mirrors out_burst/in_burst's own established "share a bundle,
 * get a separate control register" precedent, see that comment elsewhere
 * in this file). Real board measurement (ZHR-92, same round) found GELU's
 * real per-element cost is 8.60x a naive II=1/1-wide floor and ADD's is
 * 16.57x, BOTH flat across a 32x element-count range (confirmed per-
 * element, not fixed dispatch overhead) -- neither op had ever used
 * burst_maxi before this, both used plain act_t[] pointer accesses
 * (one un-bursted AXI transaction per element), the exact mechanism this
 * project's own PW ROW_READ/WRITEOUT already fixed for PW's own AXI
 * paths. ADD's extra ~2x over GELU matched its own confirmed HLS
 * diagnostic (`Unable to schedule bus request operation... due to limited
 * memory ports`, achieved II=2 from reading in_off/in2_off simultaneously
 * from the same bundle) -- whether switching to burst_maxi's own
 * sequential read_request/read calls incidentally resolves this is
 * measured, not assumed (see run_add's own comment below). */
void mac_array_top(
    LayerDescV2 desc,
    const act_t  in_base[],
    const wt_t   w_base[],
    const acc_t  b_base[],
    act_t        out_base[],
    int          *out_written,
    const ap_uint<32> in_base_wide[],
    hls::burst_maxi<ap_uint<32> > out_burst,
    hls::burst_maxi<ap_uint<32> > in_burst,
    hls::burst_maxi<ap_uint<32> > elemwise_in_burst,
    hls::burst_maxi<ap_uint<32> > elemwise_out_burst,
    hls::burst_maxi<ap_uint<32> > dw_in_burst)
{
#pragma HLS INTERFACE s_axilite port=desc     bundle=control
/* ZHR-92 round (2026-09-04): COSIM_DEPTH_HINT -- RTL cosimulation (unlike
 * synthesis or csim) needs an explicit `depth=` on each m_axi port to
 * know how many elements to transfer between the C testbench's array and
 * the RTL simulation's memory model ("A depth specification is required
 * for interface port 'in_base' for cosimulation" -- the exact HLS
 * diagnostic, not guessed). depth= is a simulation-only hint -- it does
 * NOT affect synthesized RTL (real hardware is a full AXI master
 * addressing any location, no fixed bound), confirmed by Xilinx's own
 * documentation for this pragma, so this is safe to add without altering
 * the deployed design's own synthesis output; guarded behind this ifdef
 * anyway so the default build's source is textually unchanged. Sizes
 * below match this round's own cosim testbench (pw_cosim_b1_tb.cpp,
 * pwburst_b1_t16: cin=48,cout=48,h=8,w=32) -- NOT general-purpose bounds,
 * only sized for this one cosim run. */
#ifdef COSIM_DEPTH_HINT
#pragma HLS INTERFACE m_axi port=in_base      offset=slave bundle=gmem_act depth=12288
#pragma HLS INTERFACE m_axi port=w_base       offset=slave bundle=gmem_w   depth=2304
#pragma HLS INTERFACE m_axi port=b_base       offset=slave bundle=gmem_b   depth=48
#pragma HLS INTERFACE m_axi port=out_base     offset=slave bundle=gmem_act depth=12288
#else
#pragma HLS INTERFACE m_axi port=in_base      offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=w_base       offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=b_base       offset=slave bundle=gmem_b
#pragma HLS INTERFACE m_axi port=out_base     offset=slave bundle=gmem_act
#endif
#pragma HLS INTERFACE s_axilite port=out_written bundle=control
/* ZHR-92 angle-B step (2026-08-24): out_burst is hls::burst_maxi<ap_uint
 * <32>>, matching gmem_act's real 32-bit AXI width (forced by
 * in_base_wide already sharing this bundle) -- the previous attempt used
 * <act_t> (8-bit) and crashed csynth's codegen. Same bundle=gmem_act, not
 * a 5th master. Used only by pw_flat_pipeline's WRITEOUT; the other 7
 * out_base call sites (WRITEOUT_DW, run_add/gap/relu/sigmoid/gelu/scale)
 * are untouched, still plain-pointer out_base. */
#ifdef COSIM_DEPTH_HINT
#pragma HLS INTERFACE m_axi port=out_burst    offset=slave bundle=gmem_act depth=3072
#else
#pragma HLS INTERFACE m_axi port=out_burst    offset=slave bundle=gmem_act
#endif
#pragma HLS INTERFACE s_axilite port=out_burst bundle=control
/* A3 row-hoist round (2026-08-25, ZHR-92): in_burst is ROW_READ's read-side
 * counterpart to out_burst, same bundle=gmem_act, same "shares a bundle,
 * does NOT share a control register" caveat as out_burst's own history --
 * this gets its own AXI-Lite base-address register, which the host driver
 * must program explicitly before dispatch (not yet wired into
 * mac_array_driver.c at this stage -- csim has no register-address concept
 * and cannot catch a missing write; this is a P&R-stage TODO, tracked, not
 * silently deferred). */
#ifdef COSIM_DEPTH_HINT
#pragma HLS INTERFACE m_axi port=in_burst     offset=slave bundle=gmem_act depth=3072
#else
#pragma HLS INTERFACE m_axi port=in_burst     offset=slave bundle=gmem_act
#endif
#pragma HLS INTERFACE s_axilite port=in_burst bundle=control
    /* ELEMWISE_BURST (see this function's own header comment): same
     * bundle=gmem_act as in_burst/out_burst above, same "own control
     * register, not yet wired into mac_array_driver.c" caveat. act_t
     * (8-bit) element width, not ap_uint<32> -- GELU/ADD's own natural
     * per-element granularity, avoiding out_burst/in_burst's own 4-byte-
     * word pack/unpack entirely. */
#ifdef COSIM_DEPTH_HINT
#pragma HLS INTERFACE m_axi port=elemwise_in_burst  offset=slave bundle=gmem_act depth=12288
#pragma HLS INTERFACE m_axi port=elemwise_out_burst offset=slave bundle=gmem_act depth=12288
#else
#pragma HLS INTERFACE m_axi port=elemwise_in_burst  offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=elemwise_out_burst offset=slave bundle=gmem_act
#endif
#pragma HLS INTERFACE s_axilite port=elemwise_in_burst  bundle=control
#pragma HLS INTERFACE s_axilite port=elemwise_out_burst bundle=control
    /* ZHR-92 round (2026-09-07): DWR_INPUT_BURST -- same bundle=gmem_act
     * as in_burst/out_burst/elemwise_*_burst above, same "own control
     * register, not yet wired into mac_array_driver.c" caveat -- and per
     * this project's own "flagging a risk in a comment is not the same
     * as handling it" lesson (ELEMWISE_BURST's real board hang), this is
     * a hard requirement before ANY board dispatch, not a nice-to-have:
     * add MAC_DW_IN_BURST_LO/HI register writes to every real ARM call
     * site before testing on real hardware. ap_uint<32> (word-packed),
     * not act_t (8-bit) -- the 8-bit/32-bit mixed-width crash ELEMWISE_
     * BURST hit is a codegen-level issue with the bundle itself, not
     * specific to GELU/ADD, so this port uses the same 32-bit width from
     * the start. */
#ifdef COSIM_DEPTH_HINT
#pragma HLS INTERFACE m_axi port=dw_in_burst  offset=slave bundle=gmem_act depth=4096
#else
#pragma HLS INTERFACE m_axi port=dw_in_burst  offset=slave bundle=gmem_act
#endif
#pragma HLS INTERFACE s_axilite port=dw_in_burst bundle=control
    /* ZHR-92 round (2026-09-11): DW's own packed word writeout
     * (dwr_writeout_packed) does NOT get its own port -- it REUSES
     * out_burst above. A dedicated 6th port (dw_out_burst) was built and
     * real-P&R'd on 2026-09-08 and cost -0.25ns of WNS (-0.167 -> -0.418
     * route_design alone) for a root cause that was measured, not guessed:
     * the extra port widened gmem_act's own store-unit write-request FIFO
     * (mem_reg[5][65] vs [64]) and deepened its arbitration logic, landing
     * it on the marginal path into the shared multiplier. DW and PW are
     * strictly mutually exclusive (run_layer early-returns for DW), so one
     * port serves both -- and this also removes the new-AXI-Lite-register
     * requirement entirely, sidestepping the "shared bundle != shared
     * control register" trap rather than having to handle it. */
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
#ifdef COSIM_DEPTH_HINT
#pragma HLS INTERFACE m_axi port=in_base_wide offset=slave bundle=gmem_act depth=3072
#else
#pragma HLS INTERFACE m_axi port=in_base_wide offset=slave bundle=gmem_act
#endif
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
     * distance-targeted fix -- SUPERSEDED 2026-08-29: gmem_meta itself is
     * gone now (option F, desc/out_written moved to s_axilite, see the
     * function-header comment above), so this whole distance-to-run_layer
     * problem is moot for desc, not just mitigated. */
    /* ZHR-92 round (2026-09-12): SCALAR_OP_SIZE_HOIST -- the six scalar
     * ops used to each compute their own element count inside their own
     * body (RELU/SIGMOID/GELU/ADD: cin*h_in*w_in, two multiplies each;
     * GAP/SCALE: h_in*w_in, one each -- 10 multiply call sites total),
     * every one of them reachable only through this op_type switch. HLS
     * bound them onto the top-level shared 32x32 multiplier
     * (mul_32s_32s_32_2_1, shared with run_layer and run_gelu) and put
     * the op_type test INTO the multiplier's operand mux -- one arm gated
     * by an inline 32-bit `desc_op_type == 4 | == 5` compare
     * (cin*w_in), another by FSM state (w_in*h_in) -- confirmed by
     * reading the exported RTL's own mux blocks, not inferred. That sink
     * is the critical path of every thin-margin build on this line
     * (deployed baseline: -0.167ns route_design-alone, only closes via
     * phys_opt), and the 2026-09-12 port-reuse round showed two
     * pre-existing near-tied arms into it swapping places under
     * placement with ~0.3ns of variance -- i.e. the zero margin is in the
     * sink itself, not in whatever the round changed. Computing hw and
     * total ONCE here, unconditionally (2 multiplies, no op_type
     * predicate on either), and passing them in as scalar parameters
     * (same technique as the earlier d.cin/d.cout -> scalar-parameter
     * change) removes all 10 op_type-gated call sites from the shared
     * multiplier's opset -- verified against the binding database, see
     * CLAUDE.md. Done as its own single-variable round on the deployed
     * baseline (DW_OUTPUT_BURST OFF) so its effect on that sink's margin
     * is measured cleanly. */
    /* SHARED_MUL_ARMS round (2026-09-12): the hoisted scalar_hw was still
     * one arm (the unconditional w_in*h_in @state2) of the top-level 2-arm
     * mux into the shared 32x32 multiplier. Narrow operand types (real
     * network: h_in/w_in <= 128, cin <= 1152; declared 9/20/11 bits, see
     * below, asserted in csim) make HLS emit a different, narrow
     * multiplier core that cannot share with mul_32s_32s_32 -- so the
     * top-level mux disappears by construction, without relying on an
     * ALLOCATION pragma (silently ignored once on this project). Whether
     * HLS actually keeps them narrow is verified in the binding DB, not
     * assumed. */
    /* Widths: h_in 9 bits (real max 128), w_in 20 bits (real max 128, but
     * gelu_add_burst_tb.cpp's synthetic shapes put the whole element count
     * in w_in, up to 786,432 -- kept, it's the GELU/ADD regression suite),
     * cin 11 bits (real max 1152); products 21 / 32 bits. Any of these
     * would be silently truncated for an out-of-range descriptor, so the
     * ranges are asserted in csim. */
#ifndef __SYNTHESIS__
    assert(desc.h_in >= 0 && desc.h_in < (1 << 9) && desc.w_in >= 0 && desc.w_in < (1 << 20) && desc.cin >= 0 && desc.cin < (1 << 11));
    assert((long long)desc.h_in * desc.w_in < (1LL << 21));
    assert((long long)desc.cin * desc.h_in * desc.w_in < (1LL << 31));
#endif
    const ap_uint<9>  sz_h = desc.h_in;
    const ap_uint<20> sz_w = desc.w_in;
    const ap_uint<11> sz_c = desc.cin;
    const ap_uint<21> scalar_hw_n    = sz_h * sz_w;
    const ap_uint<32> scalar_total_n = sz_c * scalar_hw_n;
    const int scalar_hw    = (int)scalar_hw_n;
    const int scalar_total = (int)scalar_total_n;
    switch (desc.op_type) {
        case LDESC_OP_ADD:     run_add(desc, scalar_total, elemwise_in_burst, elemwise_out_burst); break;
#ifdef SE_BURST
        case LDESC_OP_GAP:     run_gap(desc, scalar_total, scalar_hw, elemwise_in_burst, out_base); break;
#else
        case LDESC_OP_GAP:     run_gap(desc, scalar_hw, in_base, out_base); break;
#endif
        case LDESC_OP_RELU:    run_relu(desc, scalar_total, in_base, out_base); break;
        case LDESC_OP_SIGMOID: run_sigmoid(desc, scalar_total, in_base, out_base); break;
#ifdef SE_BURST
        case LDESC_OP_SCALE:   run_scale(desc, scalar_total, scalar_hw, elemwise_in_burst, elemwise_out_burst); break;
#else
        case LDESC_OP_SCALE:   run_scale(desc, scalar_hw, in_base, out_base); break;
#endif
        case LDESC_OP_GELU:    run_gelu(desc, scalar_total, elemwise_in_burst, elemwise_out_burst); break;
        default:                run_layer(desc, in_base, w_base, b_base, out_base, in_base_wide, out_burst, in_burst, dw_in_burst); break;
    }
    *out_written = 1;
}
