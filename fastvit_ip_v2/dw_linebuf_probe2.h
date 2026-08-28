// dw_linebuf_probe2.h -- ZHR-92 Phase 1 (2026-08-28): extends the original
// dw_linebuf_probe (v5, csynth-only, K=7/MAC_PD-sweep, correctness
// explicitly out of scope) to answer this round's actual question: does
// the 707 LUT/P slope and the mechanism itself survive contact with the
// REAL network's shapes -- runtime K in {3,7}, runtime stride in {1,2},
// real per-layer W_in, AND real same-padding boundary zero-fill (the
// original probe skipped all four).
//
// Design choices, each picked to avoid the specific "runtime value costs
// real hardware" traps this project has hit before (CLAUDE.md's own
// enumerated list):
//   - K, S, h_in, w_in are runtime function arguments, used ONLY as loop
//     bounds (for kh<K, for pr<h_pad, etc.) -- never as a multiply operand.
//   - The flat input-pixel read address is a loop-carried +1 accumulator
//     (increments once per in-bounds real pixel read), not row*w_in+col.
//   - Stride gating (row%S==0 / col%S==0) and the output index (row/S,
//     col/S) are both wrap-pair counters (phase counter 0..S-1, output
//     counter incremented only when phase==0), not a runtime %/÷ S.
//   - Same-padding boundary zero-fill: the raster iterates the PADDED
//     coordinate space (h_in+2*pad by w_in+2*pad, pad=K/2, both loop
//     bounds runtime) and gates the pixel VALUE (zero when outside the
//     real image), never the loop's own iteration count for validity --
//     matching the DW_PATCH_STAGE precedent (gate values, not indices).
#ifndef DW_LINEBUF_PROBE2_H
#define DW_LINEBUF_PROBE2_H

#include <ap_int.h>
#include <hls_stream.h>

typedef ap_int<8>  act_t;
typedef ap_int<8>  wt_t;
typedef ap_int<32> acc_t;

#define MAX_K      7                 /* compile-time array bound only */
#ifndef MAC_PD
#define MAC_PD     1                 /* channel parallelism -- swept
                                         externally (-D), same convention
                                         as v5's own MAC_PD sweep */
#endif
#define W_MAX      128               /* largest real W_in (stem) */
#define H_MAX      128
#define PAD_MAX    (MAX_K / 2)
#define WPAD_MAX   (W_MAX + 2 * PAD_MAX)
#define MAX_PIXELS (H_MAX * W_MAX)   /* synthetic input buffer, same scope
                                        limitation as v5 -- no DRAM, see
                                        gap (a) in the Phase-1 report */
#define MAX_OUT    (64 * 64)         /* worst real h_out*w_out across the
                                        real network's 25 DW layers */

struct lb_beat_t {
    act_t window[MAC_PD][MAX_K][MAX_K];
    bool  valid;
};

// out[dd][i] for i in [0, *out_count) holds the i-th valid (post-stride)
// output position's accumulated sum, in raster order of the OUTPUT grid.
// *out_count is returned so the testbench can size its own comparison.
void dw_linebuf_probe2(
    const act_t pixels[MAC_PD][MAX_PIXELS],
    const wt_t  weight[MAC_PD][MAX_K][MAX_K],
    acc_t out[MAC_PD][MAX_OUT],
    int h_in, int w_in, int K, int S,
    int *out_count
);

// fixed-shape variant (H_IN=W_IN=64, K=3, S=1, real layer_0003_dwconv
// shape) for a determinate-trip-count csynth run -- see its own comment
// in dw_linebuf_probe2.cpp.
void dw_linebuf_probe2_fixed(
    const act_t pixels[MAC_PD][MAX_PIXELS],
    const wt_t  weight[MAC_PD][MAX_K][MAX_K],
    acc_t out[MAC_PD][MAX_OUT],
    int *out_count
);

#endif
