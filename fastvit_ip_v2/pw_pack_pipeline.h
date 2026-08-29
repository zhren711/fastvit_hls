// pw_pack_pipeline.h -- ZHR-92 DSP-packing line, TERMINAL STATUS as of
// 2026-08-29: mechanism confirmed CORRECT (Step 1: pw_pack_reduce.cpp,
// 4/4 byte-exact vs real weights/data across n_cbase=1/partial/exact and
// cout=48-minimum cases; Step 3: this file's own isolated csynth, II=1
// exact, trip count exact match to theory) but INTEGRATION IS BLOCKED,
// for two independent reasons, and this line is CLOSED, not paused:
//   1. Vitis HLS 2024.2 export_design bug: combining this file as a
//      THIRD separately-compiled source (alongside
//      mac_array_raster_pwpack_integrated.cpp + dw_raster_layer.cpp) hit
//      a reproducible RTL-codegen naming mismatch (csynth log claims
//      generating core module 'mul_32s_31s_32_2_1', never actually
//      written to disk -- see CLAUDE.md's "7th confirmed instance of
//      tool reports success" entry) in a downstream module
//      (dwr_consume10) that doesn't even call this file's code. Two
//      targeted variants (removing dsp_pack_mul's BIND_OP DSP binding;
//      widening dsp_pack_mul_signed's narrow ap_uint<4>/<5>/<9>
//      intermediates to uniform ap_int<32>) both failed identically --
//      ruling out this file's own width/binding choices as the trigger.
//      Inlining the packed-reduction logic directly into
//      mac_array_raster_pwpack_integrated.cpp (eliminating the third
//      compilation unit) got PAST this bug at the csynth level -- but
//      see reason 2 below for why that path was abandoned before ever
//      being export/P&R-verified.
//   2. Whole-IP II regression: this file's own Step 3 isolated csynth
//      achieved II=1 exactly for PW_FLAT_PACKED. Compiling the SAME
//      unmodified function body inside the full mac_array_top call graph
//      (the inlined variant from reason 1) regressed to II=2 -- the
//      packed reduction's own dsp_pack_mul_signed calls, isolated to one
//      call site in Step 3, compete with the rest of the design once
//      integrated (see CLAUDE.md's isolated-csynth-can't-predict-whole-
//      IP lesson, 3rd confirmed instance). Net effect: DSP packing's
//      already-modest 2x theoretical benefit (only n_ot halving --
//      corrected from an earlier, wrong "4x" estimate; the 16 spatial
//      lanes were already UNROLL-parallel) collapses to roughly 1x once
//      II=2 is applied -- and PW's own reduction phase is only ~18% of
//      total network time, so even a clean 2x here would cap at ~-8%
//      network-wide. This alone was judged sufficient to close the line
//      without spending an export/P&R cycle to chase reason 1 further.
// Code is kept in the repo (not deleted) as a documented, verified-
// correct-but-blocked mechanism -- #ifdef PW_ENABLE_PACKING (default
// OFF everywhere) keeps it fully inert; the deployed/verified build is
// unaffected (mac_array.cpp, mac_array_raster_integrated.cpp,
// dw_raster_layer.{h,cpp} were never touched by this investigation --
// confirmed via empty git diff, and by a fresh full 17-phase csim
// regression against mac_array.cpp passing clean, 2026-08-29).
//
// Below is the ORIGINAL Step 2+3 header, preserved for design-rationale
// context:
//
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
