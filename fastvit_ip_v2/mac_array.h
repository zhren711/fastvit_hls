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
 * Scope of this PoC (2026-08-16, first round): prove two things csim-only,
 * before any synthesis/P&R:
 *   1. A layer controller driving this array can correctly execute a real
 *      DW conv followed by a real PW conv from descriptor data alone.
 *   2. The two hard requirements ZHR-91 row 6 sets for ANY new engine,
 *      motivated directly by the Add write-back defect that was never
 *      root-caused on the old architecture:
 *        a. self-verified writeback -- proven in mac_array_tb.cpp via
 *           fault injection: the harness must independently detect a
 *           silently-dropped write, not just trust AP_DONE.
 *        b. descriptor-to-MAC-array-parameter mapping is independently
 *           checked -- derive_mac_array_params() below is cross-checked
 *           by a SEPARATELY authored implementation,
 *           tools/verify_mac_array_mapping.py, not just trusted because
 *           it compiles.
 *============================================================*/

#include "ap_int.h"
#include <cstdint>

typedef ap_int<8>   act_t;   /* activation, matches fastvit_ip's act_t */
typedef ap_int<8>   wt_t;    /* weight */
typedef ap_int<32>  acc_t;   /* accumulator / bias */

/* pr x pc x pd = 8x8x8 time-multiplexed MAC array (ZHR-63 Phase A array
 * geometry, confirmed 2026-08-16 -- kept at 8x8x8, not shrunk to match
 * Stage4/FinalDW's lower utilization there). */
#define MAC_PR 8   /* output-row tile size   */
#define MAC_PC 8   /* output-col tile size   */
#define MAC_PD 8   /* output-channel tile size */

#define LDESC_OP_DWCONV 0
#define LDESC_OP_PWCONV 1

/* Layer descriptor -- structurally the same fields as
 * tools/gen_layer_descriptor.py's JSON output (step 2a), just a C struct
 * instead of JSON so it can be read straight out of DRAM by the layer
 * controller. Addresses are element offsets into the flat arrays passed
 * to mac_array_top (a PoC simplification over real byte addresses in
 * DRAM -- real Phase A wiring will use byte addresses like the current
 * driver does, this keeps the csim testbench simple). */
struct LayerDescV2 {
    int op_type;             /* LDESC_OP_DWCONV | LDESC_OP_PWCONV */
    int cin, cout;
    int h_in, w_in;
    int k, stride, pad, fpg; /* fpg unused by PW (always 1), kept for DW parity with fastvit_ip's descriptor fields */
    int out_shift;
    int in_off, w_off, b_off, out_off;  /* element offsets into in_base/w_base/b_base/out_base */
};

/* What the layer controller actually derives from a LayerDescV2 to program
 * the MAC array's loop bounds. THIS is the mapping ZHR-91 row 6 requires
 * independent verification of -- see tools/verify_mac_array_mapping.py,
 * which recomputes every field here from its own copy of the arithmetic,
 * not by importing this function. */
struct MacArrayParams {
    int h_out, w_out;
    int n_row_tiles, n_col_tiles, n_ch_tiles;   /* ceil(dim / tile_size) */
    int last_row_tile, last_col_tile, last_ch_tile;  /* remainder tile size (1..8) */
};

MacArrayParams derive_mac_array_params(const LayerDescV2 &d);

/* Layer controller + MAC array top function. Executes n_layers descriptors
 * back-to-back against the shared flat DRAM-model arrays. out_written[i]
 * is set to 1 by this function once (and only once) it has performed the
 * real output write for layer i -- FAULT_INJECT_SKIP_WRITE in
 * mac_array_tb.cpp overrides this per-layer to reproduce the Add defect's
 * symptom (IP completes, output silently not written) so the testbench's
 * independent verification step can be proven to catch it. */
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
