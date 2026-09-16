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
#define DWR_ROWBUF_WORDS (DWR_W_MAX / 4)   /* DWR_ROWBURST: lane-1 per-row word buffer (real fpg=2 layers use <= 8) */
/* DWR_WBURST: a K^2-byte kernel starting at any byte offset spans at most
 * ceil((3 + 49)/4) = 13 words; 14 keeps one spare. */
#define DWR_KW_MAX 14
/* DWR_FLAT: rows' worth of write_requests kept in flight before a response is
 * popped (<= (DWR_FLAT_DEFER_ROWS+1)*fpg = 8 outstanding vs the adapter's 16). */
#define DWR_FLAT_DEFER_ROWS 3

// ZHR-92 (2026-09-15): DWR_WBURST is ON BY DEFAULT as of the mac_array_a3_wburst
// deployed baseline. The 32-bit w_burst port (added for PW_WHOIST_WIDE) makes
// gmem_w's access width 32-bit for every port, which cost w_base's byte reads
// their K^2 kernel burst (DW +17.3ms on the board, +183/+605 cycles per channel
// at k=3/k=7); reading the kernel + shift through w_burst instead restores it
// (DW 66.4 -> 67.6ms, +27 cycles/channel residual = the second request + the
// 14-iteration word loop vs the old single burst). Define DWR_WBURST_OFF to
// revert (only sensible together with PW_WHOIST_WIDE_OFF).
#ifndef DWR_WBURST_OFF
#define DWR_WBURST 1
#endif

// ZHR-92 (2026-09-13): DW_OUTPUT_BURST is ON BY DEFAULT as of the
// mac_array_a3_dwob deployed baseline (real board: DW 531.13ms -> 329.0ms,
// -38.0%; full network 1,095.36ms -> ~898ms, -18%; byte-exact, ONNX cosine
// exact). It packs each lane's 4 output bytes into one ap_uint<32> word
// write through PW_FLAT's existing out_burst port (no 6th gmem_act port,
// no new driver register), taking dwr_consume's CROW_CCOL from achieved
// II=8 to II=2. It had failed real P&R twice (-0.418/-0.461ns) purely on
// the top-level shared 32x32 multiplier sink, which the SHARED_MUL_ARMS
// cleanup (2026-09-12) removed structurally; on top of that it closed at
// +0.172ns route_design alone. Define DW_OUTPUT_BURST_OFF to get the old
// 4-elemental-write path back (the deployed behaviour before 2026-09-13).
// NOTE: the wbuf ARRAY_PARTITION pragma inside dwr_consume rides along
// under the same macro; on its own it never changed achieved II (verified
// 2026-09-08: II stayed 8 with only the partition applied) -- it is a
// companion of the packed write, not an independent gain.
#ifndef DW_OUTPUT_BURST_OFF
#define DW_OUTPUT_BURST 1
#endif

// ZHR-92 (2026-09-14): DWR_ROWBURST is ON BY DEFAULT as of the
// mac_array_a3_rowburst deployed baseline (real board: DW 329.0 -> 224.7ms,
// -31.7%; full network ~898 -> ~790ms, -12%; byte-exact, ONNX cosine exact).
// Requires DW_OUTPUT_BURST. It moves the packed write's request and response
// from "once per 4 outputs, inside the CROW_CCOL iteration" (each write
// waited ~30 cycles for its own B response inside the II=2 pipeline: 522,240
// waits, ~156ms) to once per lane-ROW outside the pipelined loop; lane 1's
// words go through a per-row buffer drained after lane 0's burst (AXI data
// order = AW order), which also leaves ONE bus write per iteration and took
// CCOL from achieved II=2 to II=1. Define DWR_ROWBURST_OFF to get the
// per-word form back. See dw_raster_layer.cpp's DWR_ROWBURST comment.
#if defined(DW_OUTPUT_BURST) && !defined(DWR_ROWBURST_OFF)
#define DWR_ROWBURST 1
#endif

// ZHR-92 (2026-09-14): DWR_ROWREAD is ON BY DEFAULT as of the
// mac_array_a3_rowread deployed baseline (real board: DW 224.6 -> 115.5ms,
// -48.6%; full network ~790 -> ~679ms, -14%; byte-exact, ONNX cosine exact).
// The input-side twin of DWR_ROWBURST: one read_request(row_addr, w_in/4)
// per in-image row before dwr_produce's COL loop, word-packed read() inside
// it (COL II=1, iteration latency 12 -> 3), through the already-present
// dw_in_burst port (left behind by the rejected DWR_INPUT_BURST round, driver
// register 0x100 already wired). Independent of DW_OUTPUT_BURST/DWR_ROWBURST.
// Mutually exclusive with DWR_INPUT_BURST (both replace dwr_produce's read).
// Define DWR_ROWREAD_OFF to get the per-pixel in_base[] read back.
#if !defined(DWR_INPUT_BURST) && !defined(DWR_ROWREAD_OFF)
#define DWR_ROWREAD 1
#endif

// ZHR-92 (2026-09-15) DWR_ROW_PF / DWR_DEFER_WRESP -- ON BY DEFAULT as of the
// mac_array_a3_dwrow deployed baseline (real board: DW 115.6 -> 79.0ms, -31.7%;
// full network ~378 -> ~343ms, -9.3%; byte-exact, ONNX cosine exact; WNS
// +0.211 route-only, real LUT +634 vs isolated +626). Define DWR_ROW_PF_OFF /
// DWR_DEFER_WRESP_OFF to revert either. History below is as written during
// the step-1 round. Target: the per-ROW fixed cost
// of the DW DATAFLOW pair. Refit of the merge4 full-network run with a per-row
// term (R^2 0.879 -> 0.977; per-channel term drops to ~0): DW 115ms = 0.89
// cycles/input-pixel + 0.59/output + ~73 cycles per padded row x 97,344 rows
// (~71ms). Per row, both sides currently pay one AXI latency that OVERLAP each
// other but are each serial within their own process: dwr_produce issues the
// row's read_request only after pushing the previous row's last beat and then
// blocks on the first read() (~35 cycles); dwr_consume pops the row's
// write_response() right after its last write() (~30 cycles). Removing one
// side alone only exposes the other (the ROWBURST -> ROWREAD lesson), so the
// two are gated separately but meant to be measured together:
//   DWR_ROW_PF     -- dwr_produce issues the read_request for in-image row
//                     r+1 at the start of row r (prime before the ROW loop);
//                     read() stays inside the pipelined COL loop. <= 2
//                     outstanding, <= 64 words buffered (adapter: 16 / 256).
//                     Same shape as PW_ROWREAD_PREFETCH (PF=1 row).
//   DWR_DEFER_WRESP-- dwr_consume pops a row's write_response()s two valid
//                     rows later (DWR_WRESP_DEFER_ROWS), <= 2*fpg = 4 in flight
//                     (adapter 16); drained after CROW. write_request/write
//                     unchanged. Same shape as PW_DEFER_WRESP.
// Requires DWR_ROWREAD (PF) and DW_OUTPUT_BURST+DWR_ROWBURST (DEFER).
#define DWR_WRESP_DEFER_ROWS 2
#if defined(DWR_ROWREAD) && !defined(DWR_ROW_PF_OFF)
#define DWR_ROW_PF 1
#endif
#if defined(DW_OUTPUT_BURST) && defined(DWR_ROWBURST) && !defined(DWR_DEFER_WRESP_OFF)
#define DWR_DEFER_WRESP 1
#endif

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
    /* ZHR-92 (2026-09-15) DWR_WBURST: the 32-bit w_burst port on gmem_w (added
     * for PW_WHOIST_WIDE) -- once it exists on the bundle, w_base's byte reads
     * lose their burst inference (the DW prologue's K^2 kernel burst went from
     * iteration latency 2 to 15, +17ms on the board), so the prologue reads
     * its kernel + shift through this port instead. Parameter unconditional
     * (a bare #ifdef around a parameter does not compose); body gated. */
    hls::burst_maxi<ap_uint<32> > w_burst,
#ifdef DW_OUTPUT_BURST
    /* ZHR-92 (2026-09-12/13): DW's packed word writeout reuses PW_FLAT's
     * own out_burst port (no 6th gmem_act port, no new driver register).
     * ON by default since 2026-09-13 -- see the DW_OUTPUT_BURST note above
     * and dw_raster_layer.cpp's own comment. */
    hls::burst_maxi<ap_uint<32> > out_burst_w,
#endif
    int cin, int cout, int h_in, int w_in,
    int K, int S, int pad, int fpg,
    int in_off, int w_off, int b_off, int out_off,
    int shift_off,
    int in_ch_stride, int out_ch_stride, int h_out, int w_out
);

#endif
