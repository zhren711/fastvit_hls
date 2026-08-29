// pw_pack_pipeline.h -- ZHR-92 DSP-packing Step 2+3 (2026-08-29). Real
// pw_flat_pipeline-shaped packed reduction: same flattened FSM-style
// single PIPELINE loop as mac_array.cpp's pw_flat_pipeline_impl (not a
// nested-loop rewrite), extended to process ot PAIRS instead of single
// ot's. Isolated csynth target this round (II + trip count) -- csim
// integration wired in via #ifdef PW_ENABLE_PACKING (see
// mac_array_raster_pwpack_integrated.cpp), no csynth/P&R on the full IP
// yet, matching Step 1's own scope discipline.
//
// Design choices, each recorded because they weren't specified before
// this round:
//   - Weight-side pairing: (ot0, ot0+1) -- same convention as Step 1's
//     pw_pack_reduce.
//   - Spatial-side pairing: consecutive flat (rr,cw) indices, matching
//     pw_flat_pipeline_impl's own [rr][cw] row-major lane_in layout and
//     Step 1's own choice.
//   - Writeout stays at PW_FLAT_WRITEOUT_ELEMS=16 steps (not 32): each
//     writeout step now writes BOTH paired channels' value at that one
//     spatial position, instead of doubling the step count. This is the
//     choice that makes the reduction phase's 2x cleanly project onto
//     total_iters regardless of n_cbase (see the plan's own concern
//     about writeout size eating into the 2x) -- but it also means two
//     scalar stores to two different out_base regions happen in the
//     SAME writeout step, the exact structural shape that cost DW
//     raster's fpg dual-lane its II=1 (write-port conflict, ZHR-92
//     2026-08-28). Whether the same thing happens here is exactly what
//     this round's csynth is for -- not assumed either way.
//   - W4A5 truncation only (top 4/5 bits of the real int8 weight/
//     activation) -- numerically wrong on purpose, this round wants
//     real latency/II, not real accuracy (same convention as Step 1).
//   - FAST_WRITEOUT (burst_maxi) path is NOT implemented for the packed
//     variant this round -- only the scalar/SLOW-equivalent store.
//     Deferred, same as DW raster's own precedent of deferring burst
//     wiring to a later round.
//   - MAC_PD=1 only (matches the real deployed value everywhere else in
//     this project this round); no channel-tiling generalization.
#ifndef PW_PACK_PIPELINE_H
#define PW_PACK_PIPELINE_H

#include "mac_array.h"

// Same real parameters pw_flat_pipeline_impl takes, minus out_burst
// (FAST_WRITEOUT not implemented this round -- see header comment).
// ot0 must be even (caller's responsibility -- always true for every
// real PW layer, n_ot=d.cout confirmed even across all 12 distinct real
// cout values, ZHR-92 plan Q3).
void pw_flat_pipeline_packed(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    int rt, int colt, int r_sz, int col_sz);

// fixed-shape wrapper (cin=32 -> n_cbase=1 exactly, cout=4 -> n_ot_pairs=2,
// theoretical total_iters = 2*(1*32+16) = 96) so csynth can resolve a
// determinate trip count instead of "?" -- ZHR-92 DSP-packing Step 3
// (2026-08-29), same methodology as Step 1's dw_linebuf_probe2_fixed.
void pw_flat_pipeline_packed_fixed(
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[]);

#endif
