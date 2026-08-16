#ifndef __MAC_ARRAY_H__
#define __MAC_ARRAY_H__

/*============================================================
 * mac_array.h -- Phase A layer-controller + 8x8x8 time-multiplexed
 * MAC array, minimal DW+PW PoC (ZHR-63/ZHR-91).
 *
 * Replaces the op_code-dispatch + 5-fixed-function-worker architecture
 * (fastvit_ip/) with a single unified compute grid a layer controller
 * feeds from a DRAM-resident layer descriptor table, one layer at a
 * time. This file intentionally does NOT reuse fastvit_ip/dwconv_worker.cpp
 * or pwconv_worker.cpp's compute bodies -- the whole point of Phase A is a
 * different execution model (tiled MAC array vs. per-op fixed-function
 * pipelines), so copying their loop structure would just reproduce the
 * thing being replaced under a new name.
 *
 * Round 1 (2026-08-16, csim): proved a layer controller driving this array
 * can correctly execute DW->PW from descriptor data, and the two ZHR-91
 * row 6 hard requirements (self-verified writeback via fault injection,
 * independent descriptor-to-parameter mapping verification) both work.
 *
 * Round 2 (2026-08-16, csynth, no pragmas): found the un-annotated PoC
 * synthesizes to ONE physical MAC unit shared across the whole tensor --
 * not a real 8x8x8 array -- so its DSP/LUT numbers didn't answer the
 * actual geometry question. Also found derive_mac_array_params() was
 * being synthesized INTO the hardware (two 32-bit runtime dividers, since
 * stride/pad/k are descriptor fields, not compile-time constants).
 *
 * Round 3 (this round): two changes in response --
 *   1. Tile-count fields (h_out/w_out/n_*_tiles/last_*_tile) are now part
 *      of LayerDescV2 itself, computed HOST-SIDE by derive_mac_array_params()
 *      (called from mac_array_tb.cpp, standing in for the real descriptor
 *      generator) before mac_array_top ever runs -- "generator decides,
 *      hardware executes" (ZHR-91's own framing for how bugs 1-4 were
 *      structurally eliminated). This removes BOTH dividers from the
 *      synthesized design, not just makes them cheaper -- they're no
 *      longer reachable from mac_array_top's call graph at all.
 *   2. MAC_UNROLL_FACTOR (compile-time, -D flag) controls how many of the
 *      512 per-tile (channel x row x col) MAC computations run in
 *      parallel per cycle, swept over {1, 64, 128, 512} across separate
 *      csynth runs (fastvit_ip_v2/run_sweep.tcl) to trace how resources
 *      grow with parallelism degree, instead of trusting any single point.
 *============================================================*/

#include "ap_int.h"
#include <cstdint>

/* ACT_BITS lets a single point be re-measured at W8A4 (paper's target)
 * instead of this PoC's default W8A8, via -DACT_BITS=4 -- see
 * run_sweep_w8a4.tcl. Weight width (wt_t) is untouched: this only tests
 * the activation-side width, matching "W8A4" naming (8-bit weight,
 * 4-bit activation). Resource-estimate-only change (2026-08-16): clip_shift's
 * clamp constants are NOT adjusted for the narrower range, since this
 * variant is not meant to be functionally validated, only synthesized for
 * a resource comparison against the W8A8 sweep. */
#ifndef ACT_BITS
#define ACT_BITS 8
#endif
typedef ap_int<ACT_BITS> act_t;   /* activation, matches fastvit_ip's act_t at ACT_BITS=8 */
typedef ap_int<8>        wt_t;    /* weight, always 8-bit (W8) */
typedef ap_int<32>       acc_t;   /* accumulator / bias */

/* pr x pc x pd = 8x8x8 time-multiplexed MAC array (ZHR-63 Phase A array
 * geometry, confirmed 2026-08-16 -- kept at 8x8x8, not shrunk to match
 * Stage4/FinalDW's lower utilization there). */
#define MAC_PR 8   /* output-row tile size   */
#define MAC_PC 8   /* output-col tile size   */
#define MAC_PD 8   /* output-channel tile size */

/* How many of the MAC_PD*MAC_PR*MAC_PC=512 per-tile MAC computations are
 * unrolled (spatially parallel) per cycle vs. left to the loop's implicit
 * iteration (temporally multiplexed). Swept 1/64/128/512 this round --
 * default 1 (fully time-multiplexed) if not overridden via -DMAC_UNROLL_FACTOR. */
#ifndef MAC_UNROLL_FACTOR
#define MAC_UNROLL_FACTOR 1
#endif

/* Compile-time bounds for the on-chip per-tile staging buffers (receptive
 * field for DW, full-Cin spatial patch for PW) -- sized for this PoC's
 * test problem (K<=3, stride<=1, Cin<=32), NOT for arbitrary real FastViT
 * layer sizes (e.g. Stage3's Cin=192 PW). Extending these bounds to cover
 * the real network is later work, not this round's. */
#define MAX_K            3
#define PATCH_R_MAX       ((MAC_PR - 1) * 1 + MAX_K)   /* stride=1 assumed */
#define PATCH_C_MAX       ((MAC_PC - 1) * 1 + MAX_K)
#define MAX_CIN_PW        32

#define LDESC_OP_DWCONV 0
#define LDESC_OP_PWCONV 1

/* Layer descriptor -- structurally the same fields as
 * tools/gen_layer_descriptor.py's JSON output (step 2a), plus (as of
 * round 3) the tile-count fields a real descriptor generator would also
 * emit, computed host-side (see derive_mac_array_params() below) so the
 * hardware never has to run division on them. */
struct LayerDescV2 {
    int op_type;             /* LDESC_OP_DWCONV | LDESC_OP_PWCONV */
    int cin, cout;
    int h_in, w_in;
    int k, stride, pad, fpg; /* fpg unused by PW (always 1), kept for DW parity with fastvit_ip's descriptor fields */
    int out_shift;
    int in_off, w_off, b_off, out_off;  /* element offsets into in_base/w_base/b_base/out_base */

    /* host-precomputed (round 3) -- NOT computed by mac_array_top. */
    int h_out, w_out;
    int n_row_tiles, n_col_tiles, n_ch_tiles;        /* ceil(dim / tile_size) */
    int last_row_tile, last_col_tile, last_ch_tile;  /* remainder tile size (1..8) */
};

/* Host-side utility (stands in for the real descriptor generator, e.g.
 * tools/gen_layer_descriptor.py) that fills in LayerDescV2's tile-count
 * fields from its shape fields. NOT called from mac_array_top / not part
 * of the synthesized design -- only mac_array_tb.cpp calls this, the same
 * way a real driver would call the Python generator once, offline, not
 * per-inference. tools/verify_mac_array_mapping.py independently
 * re-derives the same arithmetic from scratch and diffs against what this
 * function (via the testbench's dump) actually produced -- ZHR-91 row 6's
 * mapping-verification requirement, now checking "did the host compute
 * the descriptor correctly" rather than "did the hardware".*/
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
