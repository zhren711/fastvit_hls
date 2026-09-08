// dw_raster_layer.h -- ZHR-92 Phase 1 Step 2 (2026-08-28): a real,
// descriptor-driven, single-DW-layer raster mechanism -- the "DW becomes
// an independent top-level branch in run_layer, one raster pass per
// layer per ot, before the (rt,colt) tile loop" shape from
// run_layer_raster_streaming_design_v1.md, generalizing Step 1's
// dw_linebuf_probe2 from a synthetic-shape probe to real gmem-style flat
// buffers + real LayerDescV2-shaped fields (fpg included).
//
// fpg design (worked through before writing this, per instruction):
// produce (line-buffer/window formation) is entirely input-channel-
// scoped -- it doesn't know or care how many output channels read the
// window it hands off. fpg therefore touches ONLY the consume side: each
// beat's window gets reduced against `fpg` independent K*K kernels
// instead of 1, producing `fpg` independent output channels
// co = ci*fpg + g (g in [0,fpg)) from the SAME window value. This is the
// same convention mac_array.cpp's existing DW path already uses (A2's
// fpg fix) and the same weight-file layout confirmed in Step 1 (weight
// bin is [cout][K][K], cout = cin*fpg, co -> ci = co/fpg). raster and fpg
// are therefore orthogonal, not in conflict: "one raster scan feeds `fpg`
// output channels" is exactly right, because the window is shared and
// only the reduction+writeout stage widens by a factor of `fpg`.
// PRODUCE's own resource cost is unaffected by fpg; CONSUME's MAC/
// accumulate/writeout cost scales by fpg (2x for this network's only real
// fpg value) -- flagged here, not yet re-measured (this round is csim
// only, no csynth).
//
// This round fixes channel-parallelism at MAC_PD=1 (today's real
// deployed value) -- one raster pass per real input channel (ot == ci),
// not a generalized MAC_PD>1 channel-tile loop. Generalizing to MAC_PD>1
// is deferred, not silently assumed to work; MAC_PD=1 is the only value
// this round's csim actually exercises.
//
// Deliberately does NOT #include mac_array.h's own act_t/wt_t/acc_t
// typedefs under a shared macro name -- reuses mac_array.h directly
// (identical typedefs, safe to redeclare) but never defines its own
// "MAC_PD" macro, avoiding the exact collision Step 1 hit combining
// mac_array.h (unconditional `#define MAC_PD 1`) with a probe header
// using its own MAC_PD parameterization in the same translation unit.
#ifndef DW_RASTER_LAYER_H
#define DW_RASTER_LAYER_H

#include "mac_array.h"

#define DWR_MAX_K    7
#define DWR_MAX_FPG  2
#define DWR_W_MAX    128
#define DWR_H_MAX    128
#define DWR_PAD_MAX  (DWR_MAX_K / 2)
#define DWR_WPAD_MAX (DWR_W_MAX + 2 * DWR_PAD_MAX)

// ZHR-92 round (2026-09-07): DWR_INPUT_BURST (OFF by default -- ATTEMPTED
// AND REJECTED, real board result: DW 531.20ms->574.22ms, +8.1% WORSE,
// not better. See dw_raster_layer.cpp's own DWR_INPUT_BURST comment for
// the full writeup and root cause -- the 7.48x floor below used a stale
// "achieved II=2" comment; the real achieved II is 8, meaning dwr_consume,
// not the input read, was always the real bottleneck) -- input-side
// burst_maxi rewrite of dwr_produce's own per-pixel in_base[] read
// (confirmed via a GELU/ADD-style decomposition to be the same "every
// element costs an AXI-transaction penalty" mechanism ELEMWISE_BURST just
// fixed: DW's real-to-adjusted-theoretical-floor ratio came back 7.48x,
// matching GELU's own 8.60x almost exactly -- this number itself turned
// out to be based on a stale claim, see above). DWR_CH_BUF_BYTES bounds a
// whole-channel on-chip
// prefetch buffer, sized to the largest real per-channel plane
// (cin*h_in*w_in is irrelevant here -- this is PER CHANNEL, so it's just
// h_in*w_in, max 128*128=16384 for layer_0001, DWR_H_MAX*DWR_W_MAX by
// construction). Verified before implementing (not assumed): in_off,
// in_ch_stride(=h_in*w_in), and every real channel's start address
// (in_off + ci*in_ch_stride) are mod4==0 for all 25 real DW layers --
// structurally guaranteed, not coincidental, since h_in/w_in are always
// powers of two (>=8) for every real layer, so no tail-byte handling is
// needed anywhere in the prefetch.
#define DWR_CH_BUF_BYTES (DWR_H_MAX * DWR_W_MAX)

// Real, descriptor-driven single-layer DW raster op. Mirrors the real
// hardware's own flat-buffer addressing convention (LayerDescV2's
// in_off/w_off/b_off/out_off, element offsets into shared in_base/w_base/
// b_base/out_base) so this is testable against mac_array_top() output on
// the exact same bytes, not a re-derivation of the address scheme.
//
//   w_base[w_off + co*K*K + kh*K + kw]      -- weight, co = ci*fpg+g
//   w_base[shift_off + co]                  -- per-channel shift (reused
//                                               bundle, matches real HW)
//   b_base[b_off + co]                      -- per-channel bias
//   in_base[in_off + ci*in_ch_stride + pos] -- real input, in_ch_stride
//                                               = h_in*w_in
//   out_base[out_off + co*out_ch_stride + i]-- real output, out_ch_stride
//                                               = h_out*w_out, written
//                                               INCREMENTALLY as each
//                                               raster position becomes
//                                               valid (K-row pipeline
//                                               delay), not batched.
void run_dw_layer_raster(
    const act_t in_base[],
    const wt_t  w_base[],
    const acc_t b_base[],
    act_t        out_base[],
    hls::burst_maxi<ap_uint<32> > dw_in_burst,
    int cin, int cout, int h_in, int w_in,
    int K, int S, int pad, int fpg,
    int in_off, int w_off, int b_off, int out_off,
    int shift_off,
    int in_ch_stride, int out_ch_stride, int h_out, int w_out
);

#endif
