// pw_pack_reduce.h -- ZHR-92 DSP-packing Step 1 (2026-08-29). NOT ADOPTED
// -- this Step 1 result is real (see below), but the whole DSP-packing
// line was later closed; see pw_pack_pipeline.h's own header for the full
// close-out writeup and CLAUDE.md's hard-stop-list for the final,
// 2026-08-31 unification with MAC_PD under the same gmem_w root cause.
// Real,
// descriptor-shaped packed-vs-unpacked PW reduction, csim only (no
// csynth, no P&R this round -- see the plan comment). Isolates exactly
// the risk the plan's own review flagged: array-wiring/pairing
// correctness (which two spatial positions pair, which two cout pair,
// behavior at real n_cbase chunk boundaries), NOT numerical accuracy
// (weights are real bytes truncated to W4A5, not re-trained/re-
// calibrated -- results are expected to be numerically wrong, only
// packed-vs-unpacked AGREEMENT on the same truncated inputs matters this
// round) and NOT signed-multiply correctness (already exhaustively
// verified, 262,144/262,144, ZHR-92 2026-08-27 -- see dsp_pack_mul_signed
// below, reused verbatim from dsp_pack_array_probe.cpp, not reinvented).
//
// Mirrors mac_array.cpp's real pw_flat_pipeline_impl structure: cout
// output channels, MAC_PR x MAC_PC=16 spatial lanes per cin step,
// last_row_tile/last_col_tile gate partial tiles (same convention as the
// real DW/PW code -- gate the VALUE, never the loop's own trip count).
// Packed variant pairs (co, co+1) for the weight side and consecutive
// flat spatial indices (2i, 2i+1) for the activation side -- a concrete
// pairing CHOICE this round makes explicit (not previously specified),
// consistent with lane_in's existing [rr][cw] row-major layout.
#ifndef PW_PACK_REDUCE_H
#define PW_PACK_REDUCE_H

#include <ap_int.h>

typedef ap_int<8>  act8_t;
typedef ap_int<8>  wt8_t;
typedef ap_int<5>  act5_t;
typedef ap_int<4>  wt4_t;
typedef ap_int<32> acc_t;

#define MAC_PR 4
#define MAC_PC 4

// W4A5 truncation -- "existing 8-bit weight truncated to 4-bit," per
// instruction: this round wants real latency, not real accuracy, so a
// simple top-bits truncation is enough. Kept as free functions so the
// testbench and both reduce variants use the exact same truncation --
// packed and unpacked must see byte-identical inputs.
inline wt4_t pw_pack_trunc_w(wt8_t w) { return (wt4_t)(w >> 4); }
inline act5_t pw_pack_trunc_a(act8_t a) { return (act5_t)(a >> 3); }

// weight: [cout][cin] flat, row-major (co-major). patch: [MAC_PR][MAC_PC][cin]
// flat, row-major (spatial-major, matching lane_in's real layout).
// acc_out: [cout][MAC_PR][MAC_PC] flat, zeroed by both functions on entry.
void pw_reduce_unpacked(
    int cin, int cout, int last_r, int last_c,
    const wt4_t weight[], const act5_t patch[], acc_t acc_out[]);

void pw_reduce_packed(
    int cin, int cout, int last_r, int last_c,
    const wt4_t weight[], const act5_t patch[], acc_t acc_out[]);

#endif
