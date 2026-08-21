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

/* round 15 (2026-08-20): pr x pc x pd = 64 physical MACs -- DELIBERATE,
 * REGISTERED reproduction deviation from the paper's literal 8x8x8=512
 * (see ZHR-64 sec.8 checklist + comment log). Rounds 11-14 proved DSP can
 * be shared across DW/PW at 512-wide (535 DSP), but every variant blew
 * LUT (2.4x+ over budget) and/or FF (up to 160%) at that width --
 * confirmed NOT a timing artifact (all csynth so far is at the loose
 * 10ns/100MHz constraint; never even attempted 200MHz), so lowering clock
 * cannot recover resources here. Dropping array width is the only lever
 * that actually shrinks LUT/FF/DSP. Goal of this width is getting a
 * complete, correct, real-hardware-measured system first (layer
 * controller/AXI-DMA not built yet); array width is revisited as an
 * independently sweepable parameter once that system exists and each
 * point can be verified on real hardware instead of csynth estimates
 * alone (this project has never run a single P&R). pd's role differs by
 * op (see the round-5 note above): DW's real parallel channel axis vs.
 * PW's reduction-tile axis.
 *
 * A2 pre-step round 2 (2026-08-21): dropped 64->32 (4x4x2). The K=7 fix
 * alone pushed LUT 45%->75% (single global MAX_K shared by every DW call,
 * not per-layer), and the design has never had any AXI/DMA interface at
 * all (every resource number through round 15 is compute-core-only,
 * confirmed by direct inspection of the HW Interfaces section -- every
 * port is ap_none/ap_vld, not m_axi). 12,898 LUT free at 64-wide can't
 * plausibly fit AXI infrastructure (ZHR-8's real 17->4 master swing alone
 * was ~14.6k LUT on this same budget). Halved PC (8->4), kept PR/PD --
 * narrower spatial tile, same channel/reduction depth, matches the
 * project's convention of changing one geometry axis at a time even when
 * the overall move (halving throughput) is itself a bigger decision. */
#define MAC_PR 4   /* output-row tile size (both ops)   */
#define MAC_PC 4   /* output-col tile size (both ops)   */
#define MAC_PD 2   /* DW: channel tile. PW: Cin reduction-chunk size. */

/* Compile-time bounds for on-chip staging buffers -- sized for this PoC's
 * test problem (Cin<=32), NOT arbitrary real FastViT layer sizes (e.g.
 * Stage3's Cin=192 PW). MAX_STRIDE=2 covers every real DW stride used in
 * FastViT-T8 (Stem + the 3 Transitions). Both are upper bounds guarded at
 * runtime inside compile-time-bounded loops, never used as a loop's own
 * runtime bound.
 *
 * A2 pre-step (2026-08-21): MAX_K was 3, silently wrong for real
 * FastViT-T8 -- direct inspection of tools/layer_descriptor_256.json
 * found 13 of the 52 real DW conv layers use K=7 (the mlp/conv/conv
 * layers and all 3 stage-downsample layers, 3 of those ALSO at stride=2),
 * not just K=3. With MAX_K=3, the tap loop (DW_TAP_H/DW_TAP_W, both
 * bounded by MAX_K) only ever covered 9 of a K=7 kernel's 49 real taps --
 * no crash, no error, just a quietly wrong convolution result on a
 * quarter of the network's DW layers. Found and fixed before any A2
 * integration code was written, not after (see the A2 design doc on
 * ZHR-63/92). PATCH_R_MAX/PATCH_C_MAX grow accordingly (9->13, 17->21 at
 * the current MAC_PR=4/MAC_PC=8); dw_wtile's per-channel K*K storage
 * grows 3x3->7x7; DW_TAP_H/DW_TAP_W's trip count grows 9->49 (K=3 layers
 * still take the same real work, just waste more `valid=false`
 * zero-weight cycles now that the shared bound is bigger -- MAX_K is one
 * global constant for every DW call, not per-layer, so this affects the
 * resource/cycle cost of K=3 layers too, not just K=7 ones). */
#define MAX_K             7
#define MAX_STRIDE        2
#define PATCH_R_MAX       ((MAC_PR - 1) * MAX_STRIDE + MAX_K)
#define PATCH_C_MAX       ((MAC_PC - 1) * MAX_STRIDE + MAX_K)
#define MAX_CIN_PW        32

#define LDESC_OP_DWCONV  0
#define LDESC_OP_PWCONV  1
#define LDESC_OP_ADD     2   /* elementwise residual add, two sources */
#define LDESC_OP_GAP     3   /* global average pool: HxW per channel -> 1 value/channel */
#define LDESC_OP_RELU    4   /* elementwise ReLU (SE block only -- confirmed via ONNX node
                               * histogram, 1 total Relu node in the whole 52-layer graph) */
#define LDESC_OP_SIGMOID 5   /* elementwise sigmoid (SE gate -- 1 total Sigmoid node) --
                               * PLACEHOLDER quantization (see run_sigmoid), not calibrated,
                               * proves the GAP->fc->act->gate data flow, not numeric accuracy */
#define LDESC_OP_SCALE   6   /* per-channel broadcast gate multiply (SE's final Mul):
                               * op0=in_off is the full HxWxC feature map, op1=in2_off is the
                               * C-length gate, broadcast over spatial -- confirmed from
                               * layer_dag_ground_truth.json: final_conv fan_out=2 feeds both
                               * ReduceMean and this Mul directly from the SAME tensor */
#define LDESC_OP_LSCALE  8   /* layer_scale: per-channel broadcast multiply, same shape as
                               * LDESC_OP_SCALE but op1 (the C-length factor) is a TRAINED
                               * WEIGHT (read from w_base at w_off) not a computed activation
                               * (in_base) -- found during the A2 design pass: 10 real layers
                               * (every mlp/fc2 output) feed a layer_scale Mul before the
                               * residual Add, and A1 never built this op because no synthetic
                               * test sequence exercised it. Confirmed distinct from SE's
                               * LDESC_OP_SCALE by consumer-tag co-occurrence in
                               * layer_descriptor_256.json ('mul' alone = layer_scale, 'mul'
                               * co-occurring with 'se_reduce' = SE's gate multiply). */
#define LDESC_OP_GELU    7   /* elementwise GELU, single source. This is the ATOMIC hardware
                               * op only -- confirmed via direct ONNX inspection that the real
                               * graph represents each GELU as a 4-node Div->Erf->Add->Mul
                               * chain (17 instances total), so a real gen_layer_descriptor.py
                               * run must fold that 4-node pattern into ONE LDESC_OP_GELU entry
                               * (old driver did this too, per ZHR-9) -- that folding is
                               * generator-side Python work, deferred to A2 when the real
                               * descriptor JSON is actually consumed. This round only builds
                               * and tests the hardware op itself via a directly-constructed
                               * descriptor, same as every other A1 op so far. */

/* Layer descriptor -- structurally the same fields as
 * tools/gen_layer_descriptor.py's JSON output (step 2a), plus the
 * host-precomputed tile-count fields (round 3: "generator decides,
 * hardware executes", removes runtime division from the synthesized
 * design). n_ch_tiles/last_ch_tile are always computed from Cin (round 5:
 * DW uses them for its real output-channel tiling -- cin==cout for
 * depthwise; PW uses them for its Cin reduction-chunk stepping).
 *
 * Phase A1 (2026-08-20): added in2_off for Add's second operand (the
 * layer_scale/processed-branch source; op0 is the existing in_off, the
 * token_mixer/identity branch -- both confirmed from
 * tools/layer_dag_ground_truth.json's multi_input_nodes, not assumed).
 * Interface-sketch note (reviewed, not yet implemented): a real m_axi
 * design will also need an in2_stride_mode bit to distinguish Add's
 * same-shape second operand from SE's channel-broadcast one -- not
 * needed yet since Add is the only two-source op this round. */
struct LayerDescV2 {
    int op_type;             /* LDESC_OP_DWCONV | LDESC_OP_PWCONV | LDESC_OP_ADD */
    int cin, cout;
    int h_in, w_in;
    int k, stride, pad;
    int fpg;  /* filters-per-group: DW's cout = cin*fpg, each input channel
               * produces fpg independent output channels (own K*K filter
               * each), still no cross-channel reduction. fpg=1 is standard
               * depthwise (cout==cin). Always 1 for PW. USED by run_layer's
               * DW path since A2's fpg=2 fix (2026-08-21) -- previously
               * carried but never read, silently wrong on the 4 real
               * layers (3 stage-downsamples + final_conv) that need it. */
    int out_shift;
    int in_off, w_off, b_off, out_off;  /* element offsets into in_base/w_base/b_base/out_base */
    int in2_off;              /* Add's second operand offset into in_base (op0=in_off, op1=in2_off) */

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
