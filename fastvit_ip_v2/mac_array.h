#ifndef __MAC_ARRAY_H__
#define __MAC_ARRAY_H__

/*============================================================
 * mac_array.h -- Phase A layer-controller + 8x8x8 MAC array, minimal
 * DW+PW PoC (ZHR-63/ZHR-91).
 *
 * Round 5 (2026-08-16): full structural rewrite after code review found
 * three independent root causes invalidating every round 3/4 resource
 * number (round 4's "ARRAY_PARTITION complete is the main cause" verdict
 * is formally withdrawn):
 *
 *   Bug 1 -- PIPELINE II=1 never actually engaged. The reduction loops
 *   (DW's kh/kw, PW's ci) had RUNTIME bounds (K, Cin -- LayerDescV2
 *   fields), so Vitis HLS could not flatten them into the OUTER loop and
 *   silently dropped the pipeline directive ("Cannot unroll loop ...
 *   variable trip count", "Unable to satisfy pipeline directive for loop
 *   'OUTER'" -- both present in round 4's own csynth log). Every "lane"
 *   really did get its own independent FSM, but because of variable trip
 *   counts, not code style.
 *
 *   Bug 2 -- confirmed via csynth instance counts, NOT the multipliers:
 *   mac_muladd instance count matched the unroll factor exactly (real
 *   MACs were fine). The LUT was per-lane control/address-generation/
 *   muxing (Expression+Multiplexer+Instance buckets, e.g. 72 separate
 *   72x281-LUT patch-address generators at factor=64, one ap_NS_fsm mux
 *   alone at 2693 LUT).
 *
 *   Bug 3 (the one that actually mattered) -- MAC_PD tiled the WRONG
 *   dimension. It tiled the output-parallel axis (PW: Cout) and left the
 *   true reduction axis (PW: Cin; DW: K*K taps) serial *inside* each
 *   lane. That makes "512 lanes" mean 512 independent full reductions
 *   (DW 512*9=4608 MACs, PW 512*32=16384 MACs) instead of the paper's
 *   literal 512 physical MAC units reused over time:
 *     PW: pd=8 tiles Cin (the reduction axis), pr*pc=64 is the output
 *         spatial tile computed in parallel -- 512 MACs/cycle producing
 *         64 partial sums, iterated Cin/8 times per output channel,
 *         Cout output channels processed one at a time.
 *     DW: pd=8 tiles the channel axis (real parallel axis, no cross-
 *         channel reduction), pr*pc=64 is spatial -- 512 lanes, one
 *         kernel tap per cycle, K*K cycles.
 *   Every round 3/4 sweep point measured a machine ~32x bigger than the
 *   one the paper's 8x8x8=512 actually describes.
 *
 *   Real correctness bug found alongside (not yet exercised by csim):
 *   PATCH_R_MAX/PATCH_C_MAX assumed stride=1 ("stride=1 assumed" comment,
 *   literally in round 3's code). Stem and all three Transitions in the
 *   real network use stride=2 DW convs; at K=3/stride=2 the true
 *   receptive field is 17x17=289, not the 10x10=100 the old bound
 *   allocated -- an actual out-of-bounds write that csim's stride=1-only
 *   test case never touched. Fixed here via MAX_STRIDE.
 *
 * This round's rewrite: every UNROLLed loop bound is now a compile-time
 * constant (MAC_PD/MAC_PR/MAC_PC/MAX_K); the only loops with
 * descriptor-derived (runtime) bounds are PIPELINE'd loops, which don't
 * need a compile-time trip count the way UNROLL does. Address arithmetic
 * is confined to staging/write-out loops, never inside the pipelined MAC
 * region. No more MAC_UNROLL_FACTOR sweep -- the design is now fixed at
 * the paper's literal 512-physical-MAC geometry; testing a "smaller"
 * point would no longer mean the same thing it did in round 3.
 *============================================================*/

#include "ap_int.h"
#include <cstdint>

typedef ap_int<8>   act_t;   /* activation, matches fastvit_ip's act_t */
typedef ap_int<8>   wt_t;    /* weight */
typedef ap_int<32>  acc_t;   /* accumulator / bias */

/* pr x pc x pd = 8x8x8 = 512 physical MACs (ZHR-63 Phase A array geometry,
 * confirmed 2026-08-16). pd's role differs by op (see the round-5 note
 * above): DW's real parallel channel axis vs. PW's reduction-tile axis. */
#define MAC_PR 8   /* output-row tile size (both ops)   */
#define MAC_PC 8   /* output-col tile size (both ops)   */
#define MAC_PD 8   /* DW: channel tile. PW: Cin reduction-chunk size. */

/* Compile-time bounds for on-chip staging buffers -- sized for this PoC's
 * test problem (Cin<=32), NOT arbitrary real FastViT layer sizes (e.g.
 * Stage3's Cin=192 PW). MAX_STRIDE=2 covers every real DW stride used in
 * FastViT-T8 (Stem + the 3 Transitions); MAX_K=3 covers every real DW
 * kernel size used. Both are upper bounds guarded at runtime inside
 * compile-time-bounded loops, never used as a loop's own runtime bound. */
#define MAX_K             3
#define MAX_STRIDE        2
#define PATCH_R_MAX       ((MAC_PR - 1) * MAX_STRIDE + MAX_K)   /* 17 */
#define PATCH_C_MAX       ((MAC_PC - 1) * MAX_STRIDE + MAX_K)   /* 17 */
#define MAX_CIN_PW        32

#define LDESC_OP_DWCONV 0
#define LDESC_OP_PWCONV 1

/* Layer descriptor -- structurally the same fields as
 * tools/gen_layer_descriptor.py's JSON output (step 2a), plus the
 * host-precomputed tile-count fields (round 3: "generator decides,
 * hardware executes", removes runtime division from the synthesized
 * design). n_ch_tiles/last_ch_tile are always computed from Cin (round 5:
 * DW uses them for its real output-channel tiling -- cin==cout for
 * depthwise; PW uses them for its Cin reduction-chunk stepping). */
struct LayerDescV2 {
    int op_type;             /* LDESC_OP_DWCONV | LDESC_OP_PWCONV */
    int cin, cout;
    int h_in, w_in;
    int k, stride, pad, fpg; /* fpg unused by PW (always 1), kept for DW parity with fastvit_ip's descriptor fields */
    int out_shift;
    int in_off, w_off, b_off, out_off;  /* element offsets into in_base/w_base/b_base/out_base */

    /* host-precomputed -- NOT computed by mac_array_top. */
    int h_out, w_out;
    int n_row_tiles, n_col_tiles, n_ch_tiles;        /* ceil(dim / tile_size); n_ch_tiles from Cin always */
    int last_row_tile, last_col_tile, last_ch_tile;  /* remainder tile size (1..8) */
};

/* Host-side utility (stands in for the real descriptor generator). NOT
 * called from mac_array_top / not part of the synthesized design.
 * tools/verify_mac_array_mapping.py independently re-derives the same
 * arithmetic from scratch and diffs against what this function (via the
 * testbench's dump) actually produced. */
struct MacArrayParams {
    int h_out, w_out;
    int n_row_tiles, n_col_tiles, n_ch_tiles;
    int last_row_tile, last_col_tile, last_ch_tile;
};
MacArrayParams derive_mac_array_params(const LayerDescV2 &d);

/* Layer controller + MAC array top function. Executes n_layers descriptors
 * back-to-back against the shared flat DRAM-model arrays. out_written[i]
 * is set to 1 by this function once (and only once) it has performed the
 * real output write for layer i -- mac_array_tb.cpp's Phase 2 overrides
 * the DRAM contents afterward (not this function) to reproduce the Add
 * defect's symptom (IP completes, output silently not written) so the
 * testbench's independent verification step can be proven to catch it. */
void mac_array_top(
    const LayerDescV2 desc[],
    int n_layers,
    const act_t  in_base[],
    const wt_t   w_base[],
    const acc_t  b_base[],
    act_t        out_base[],
    int          out_written[]
);

#endif // __MAC_ARRAY_H__
