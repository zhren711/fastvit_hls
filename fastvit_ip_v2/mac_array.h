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
#include <cassert>
#include <hls_burst_maxi.h>

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
/* A3 row-hoist timing round (2026-08-25, ZHR-92): 2->1, stop-loss after
 * five failed attempts to recover WNS on the row-hoist design via targeted
 * multiplier fixes (U2598's real source(s) never identified -- open
 * question in ZHR-92). Cost: computation roughly doubles (UNIFIED
 * 144->288 cycles/ot), but row-hoist's own H-cost cut (16,580->~1,228
 * cycles/tile) is large enough that entry3's net per-tile cost still only
 * grows 8,140->15,052 cycles (13.5x -> 7.3x vs. the pre-row-hoist
 * baseline) -- and unlike MAC_PD=2, this is expected to actually route at
 * WNS>=0, which is the only number that matters (7.3x that ships beats
 * 13.5x that doesn't). Independently sweepable parameter, same convention
 * as MAC_PC's own 8->4 history above. */
#define MAC_PD 4   /* DW: channel tile. PW: Cin reduction-chunk size. */

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

/* A3 round (2026-08-21, ZHR-92): PW's weight+bias only depend on
 * (ot,cbase), never on the (rt,colt) spatial tile -- but PW_STAGE/
 * PW_WSTAGE/the bias read all sit inside the rt/colt/ot/cbase nest, so
 * every real (rt,colt) spatial tile re-fetches the SAME weight+bias data
 * from DRAM. Measured exactly on entry[3] of the real 82-entry sequence
 * (cin=cout=48, 64x64, 4x4 tiling): weight redundancy = n_row_tiles *
 * n_col_tiles = 256.0x exactly, confirmed against the HLS burst-inference
 * log independently (PW_WSTAGE is the one path with NO burst inference at
 * all -- two separate pieces of evidence pointing at the same victim).
 * Fix: cache the layer's FULL weight+bias matrix on-chip ONCE, before the
 * rt/colt loop (loop-invariant hoist -- doesn't touch the rt/colt/ot/cbase
 * nest itself, so acc's cross-cbase accumulation and WRITEOUT timing are
 * unaffected).
 *
 * MAX_PW_WEIGHT_CACHE is NOT sized for entry 3 alone -- an earlier attempt
 * at 4096 (entry 3's own 2304-element need) silently overran on
 * mac_array_tb.cpp's own Phase12 (cin=1152,cout=384, the REAL
 * layer_0044_pwconv shape, 442368 elements), corrupting a `static` array
 * rather than crashing cleanly. 442368 is confirmed (via
 * weights_layout.h's FV_WEIGHT_SIZES) to be the largest PW weight blob in
 * the whole real 52-layer network -- sized to that exactly, a deliberate
 * ~432KB BRAM commitment (current P&R headroom: 6/280 RAMB18E1 used, but
 * that number predates this change and is NOT yet re-verified against it).
 * Activation's separate 48x redundancy needs an actual loop-nest reorder
 * (ot moved outside rt/colt), which changes acc lifetime and WRITEOUT
 * timing and is deliberately deferred to its own round, not bundled with
 * this change. */
#define MAX_PW_WEIGHT_CACHE  442368
#define MAX_PW_BIAS_CACHE    1152

/* A3 round 2 (2026-08-21, ZHR-92): the weight-hoist round's own result
 * (eliminating 587,520 bytes of DRAM traffic barely moved the needle,
 * 700.27ms -> 690.37ms) led to a corrected diagnosis: cost is per-AXI-
 * TRANSACTION, not per-byte -- PW_STAGE's innermost contiguous run is
 * only MAC_PC=4 bytes (one output row's worth of a single channel), so
 * every (rt,colt,ot,cbase) restage issues Cin*MAC_PR = 192 separate
 * 4-byte bursts. Measured: 699ms / (16*16*48 * 192) bursts = 296 ns/burst
 * (~30 cycles/burst through SmartConnect+HP+DDR3 for an unpipelined short
 * transaction) -- a normal number for THAT many small requests, not
 * evidence of a slow interconnect. Weight's DRAM traffic was already a
 * small, CONTIGUOUS blob (few bursts to begin with), so hoisting it barely
 * touched the burst count; activation's staging reruns once per (rt,colt,
 * ot,cbase) even though the data only depends on (rt,colt) -- the SAME
 * loop-invariant redundancy class weight had, just not fixed yet.
 * Retraction: the round-1 conclusion "activation's 48x isn't worth doing
 * (it cuts bytes, not iterations)" was wrong -- PW_STAGE's DRAM traffic
 * IS re-issued once per (rt,colt,ot,cbase) iteration, so 48x fewer
 * activation restages means 48x fewer bursts too, not just fewer bytes.
 *
 * Fix: same technique as the weight hoist -- stage the full-Cin spatial
 * patch for a given (rt,colt) ONCE (right after colt is known, before the
 * ot loop), reused across every ot/cbase, instead of restaging it from
 * DRAM on every one of the 48 output channels that don't change it.
 * MAX_CIN=1152 is the real network's largest channel count (same real-
 * network scoping as MAX_PW_WEIGHT_CACHE, not a PoC-only bound) -- buffer
 * is MAX_CIN*MAC_PR*MAC_PC = 18,432 bytes (~5 BRAM36 tiles), small next to
 * the weight cache's ~432KB commitment. No loop reorder, no change to
 * acc's cross-cbase accumulation or WRITEOUT timing -- exactly the same
 * risk profile the weight hoist already proved out. */
#define MAX_CIN  1152

/* A3 row-hoist round (2026-08-25, ZHR-92): PW_PATCH_HOIST row-level burst
 * hoist. MAX_CIN_TIMES_W=9216 is the real PRODUCT bound Cin*w_in across
 * every real PWCONV layer in the 82-entry network (verified directly
 * against desc_all.bin, not estimated -- entry9: cin=144,w_in=64 -> 9216
 * exactly). NOT MAX_CIN*W_MAX (1152*64=73,728, 8x larger) -- Cin and W
 * trade off against each other in the real network, so the independent
 * per-dimension bound is never simultaneously needed. row_buf's flat
 * per-channel stride is the RUNTIME w_in, not a fixed W_MAX slot, which is
 * what makes the smaller product bound achievable -- see row_buf's own
 * declaration in run_layer. mac_array_tb.cpp's desc12 (cin=1152,w_in=8)
 * and desc13 (cin=144,w_in=64) both land exactly on this bound (1152*8=
 * 144*64=9216), so csim already exercises the tight boundary without a
 * new phase. MAX_WORDS_PER_CH=17 bounds ROW_READ_FILL's per-channel word
 * count: worst case byte-offset r=3 within the first word, w_in=64 ->
 * (3+64+3)>>2=17 words to cover the run. */
#define MAX_CIN_TIMES_W  9216
#define MAX_WORDS_PER_CH 17

/* ZHR-92 round (2026-09-06): ELEMWISE_CHUNK bounds run_gelu/run_add's own
 * new burst_maxi read/write chunk size (see ELEMWISE_BURST's own header
 * comment in mac_array_raster_integrated.cpp). 4096 bytes/elements per
 * chunk -- a conservative, clearly-safe DMA burst size (no documented
 * exact hls::burst_maxi single-request length ceiling was found locally;
 * this sidesteps that uncertainty by design rather than assuming an
 * unverified large single-request length works). The real largest GELU/
 * ADD tensor (786,432 elements, layer 0's own GELU) needs 192 chunks --
 * a plain sequential runtime-trip-count outer loop, not unrolled, so no
 * compile-time-bound requirement applies to it (only the INNER per-chunk
 * loop, bounded by this constant, needs one).
 *
 * REVISED same round: the initial act_t (8-bit) burst_maxi design crashed
 * csynth's own codegen (`Call parameter type does not match function
 * signature!`, `_ssdm_op_Write.m_axi.p1i32` expecting 32-bit) -- mixing
 * an 8-bit burst_maxi port onto the SAME bundle as the existing 32-bit
 * out_burst/in_burst ports is not the same case as the already-proven
 * "plain pointer + burst_maxi share a bundle" precedent (the plain
 * pointer's own width already matches the bundle's dominant 32-bit width;
 * a genuinely different-width burst_maxi does not). Fixed by making the
 * new ports ap_uint<32>-typed (matching the bundle) instead, with 4-byte
 * pack/unpack inside run_gelu/run_add -- verified safe first (per this
 * project's own "verify contiguity/alignment before batching" rule): all
 * 27 real GELU/ADD entries have in_off/out_off/in2_off AND total (=cin*h*w)
 * exactly mod4==0 (real network channel counts and spatial dims are
 * always multiples of 4), so ELEMWISE_CHUNK stays a clean multiple of 4
 * words with zero tail-byte handling needed anywhere. */
#define ELEMWISE_CHUNK 4096
#define ELEMWISE_CHUNK_WORDS (ELEMWISE_CHUNK / 4)

/* A3 round 3 (2026-08-21, ZHR-92): bound for run_reduce_unified's
 * per-step gather buffers (lane_in_all/lane_w_all), see mac_array.cpp's
 * header comment on the drive_mac removal for the full rationale. Must
 * cover DW's real step count (MAX_K*MAX_K=49 taps) -- PW's per-cbase
 * step count (ceil(MAX_CIN_PW/MAC_PD)=16) always fits comfortably
 * within that. */
#define MAX_STEPS (MAX_K * MAX_K)

/* A3 round (2026-08-23, ZHR-92): the outer-hoisted oc_tbl_all/oc_ch_tbl_all
 * table (one entry per (ot,f,dd), computed once per run_layer call) was
 * ATTEMPTED AND REVERTED the same day -- csim 16/16 clean, but the table
 * (up to 2,304 entries x2 arrays, real network's fpg=2 worst case) grew
 * the design enough that P&R failed to ROUTE inside the X0-96 pblock (83
 * unroutable pins), and the underlying premise it was testing ("two
 * 32-bit multiplies costing 422 cycles/tile") didn't survive scrutiny
 * either -- two DSP-level 32x32 multiplies cost 3-4 cycles each, nowhere
 * near 422, even serialized with arbitration overhead. Reverted back to
 * WRITEOUT_DW's own per-(rt,colt,ot,f) local oc_tbl[MAC_PD]/
 * oc_ch_tbl[MAC_PD] (solution21's form) while the real +422 cycles/tile
 * source gets re-investigated (leading candidate: WRITEOUT_DW's own
 * Interval=33 multiplied by however many times it's actually called per
 * tile, not the precompute block itself -- see ZHR-92 for the live
 * investigation). MAX_OC_TBL is no longer used; kept here as a comment,
 * not a dangling #define, in case this direction is revisited once the
 * real cause is confirmed. */

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
/* LDESC_OP_LSCALE (layer_scale as a standalone op, value 8) was implemented
 * and verified 2026-08-21 (commit e5e1246) then removed the same day once
 * ZHR-92 confirmed Route A: gamma is folded into fc2's weight+bias at
 * export time (tools/fold_layer_scale.py, validated to ~1e-16, Phase A
 * step 2b-1), so the real 83-entry hardware sequence never dispatches a
 * layer_scale op -- fc2's own PWCONV output already carries the scale.
 * Full implementation + Phase10 csim coverage recoverable from git
 * (commit e5e1246) if a future quantization scheme (Phase C, W8A4
 * retrain) makes gamma non-foldable again. */
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

    /* A2 (2026-08-21, ZHR-92): per-channel out_shift. A single averaged
     * shift per layer saturates channels whose weight_scale is far above
     * the layer's mean (confirmed on real data: Stem's weight_scale spans
     * 396x across 48 channels, 29/48 output channels >50% saturated at
     * the shared shift; network-wide median spread is 43.7x, worst layer
     * 5507x -- not a Stem-specific quirk). Deliberately appended at the
     * END of the struct, not inserted near out_shift, so every existing
     * positional brace-initializer (all 14 csim phases predating this)
     * keeps working unmodified -- they never mention these two fields,
     * C++ aggregate init zero-fills trailing unspecified fields, and
     * use_shift_table=0 is exactly "use the old scalar out_shift",
     * i.e. zero-init IS backward compatible by construction, not by
     * convention someone has to remember. */
    int use_shift_table;     /* 0 = use out_shift (scalar, old behavior).
                               * 1 = look up w_base[shift_off + channel]
                               * per output channel instead. */
    int shift_off;           /* element offset into w_base (reused, not a
                               * new array -- shift values fit trivially
                               * in wt_t's 8 bits) of a Cout-length table,
                               * one shift value per output channel. Only
                               * meaningful when use_shift_table=1. */

    /* A3 (2026-08-21, ZHR-92): host-precomputed channel-plane strides,
     * same append-at-the-end / zero-init-is-safe convention as
     * use_shift_table/shift_off above. See MacArrayParams' in_ch_stride/
     * out_ch_stride for why -- eliminates the runtime h_in*w_in /
     * h_out*w_out multiply from run_layer's per-lane address arithmetic
     * entirely; hardware reads these fields instead. */
    int in_ch_stride, out_ch_stride;

    /* A3 round (2026-08-23, ZHR-92): gmem_act port-widening, PW read side
     * first. Same append-at-the-end / zero-init-is-safe convention as
     * use_shift_table above -- use_wide_path=0 is "use the old narrow
     * per-element path", so every existing csim case (none of which
     * mention this field) keeps working unmodified.
     * Generator-computed (gen_hw_sequence.py), not hardware-computed:
     * "generator decides, hardware executes" -- computed once from
     * w_out>=4, checked against the real 82-entry network, not assumed:
     * every real conv/pwconv layer has w_out either exactly 1 (layers 50,
     * 51 -- the SE block's 1x1 fc1/fc2, h_in=w_in=1) or a multiple of 4
     * (every other layer, col_sz=MAC_PC=4 on every tile, no partial-last-
     * tile case exists in the real network at all). Layers 50/51 need the
     * narrow path: PW_PATCH_HOIST's read address is
     * in_off + ci*in_ch_stride + oh*W+ow -- for these two layers
     * in_ch_stride=h_in*w_in=1, so channels ARE contiguous in DRAM, but
     * the loop's innermost (burst-eligible) axis is spatial (ow), which
     * only has 1 element here; widening as currently structured would
     * read 4 bytes and use 1, the same failure mode that made the DW
     * whole-block-burst attempt regress 21% earlier this line.
     *
     * A3 round (2026-08-23, ZHR-92, MERGE): NO LONGER READ by
     * PW_PATCH_HOIST -- that code merged the wide/narrow branches into
     * one unconditional word-read path (general mod-4 byte-lane
     * decomposition handles both cases without needing to know which one
     * it is). Field kept declared (harmless, zero-init-safe, avoids
     * touching every existing csim call site and the generator stub) in
     * case a future call site needs the distinction again -- not
     * currently wired to anything.
     *
     * DEPRECATED (2026-08-24, ZHR-92): confirmed dead, not just unread by
     * PW_PATCH_HOIST specifically -- grepped tools/gen_hw_sequence.py and
     * every other tools/*.py builder, none set this field at all (always
     * implicit-zero). No code anywhere in mac_array.cpp reads it either
     * (grep-confirmed). There is no "narrow path" left in this design for
     * it to select -- do not read this comment's OWN history above as
     * evidence one still exists somewhere; it doesn't. If a future round
     * needs a real narrow/wide distinction again, don't repurpose this
     * field blind -- re-derive the need first, since its last real
     * semantics (PW_PATCH_HOIST's now-removed branch) may not match what
     * a new use would need. */
    int use_wide_path;

    /* A3 shared-multiplier round (2026-08-25, ZHR-92) -- ATTEMPTED AND
     * REVERTED (a `total=cin*h_in*w_in` field here, read by run_add/
     * run_relu/run_sigmoid/run_gelu instead of each computing it fresh).
     * Genuinely eliminated the FOUR source lines the HLS binding database
     * named (mul_ln1265/1248/1217/1179, grp_fu_938's opset) from any
     * multiplier binding, and dropped mul_32s_32s_32_2_1's real instance
     * count 2->1 (U2597 gone) -- but the SURVIVING instance (U2598) is the
     * one every P&R run's critical path actually terminates at, and real
     * P&R got WORSE, not better (WNS -0.181973ns -> -0.294226ns). U2598
     * has a 5th-or-more binding source this round never found -- see the
     * follow-up investigation (grep U2598's own grp_fu_N and opset, same
     * binding-database method, not the .bind.rpt text search that
     * incorrectly seemed to show 0 occurrences project-wide -- that check
     * was invalid, .bind.rpt uses opcode labels like mul(12), never RTL
     * core names, a lesson now in CLAUDE.md). Not reintroduced here --
     * kept as a documented TODO for whoever finds U2598's real remaining
     * source(s), not a dead end. */
};

/* ================================================================
 * SUPPORTED SHAPE RANGE -- consolidated, ZHR-92 2026-08-30.
 *
 * Three independently-discovered instances of the same failure class in
 * this project's history -- "a compile-time/architectural assumption
 * silently narrower than a runtime value's real range, invisible because
 * the real network's own shapes happen to avoid it": fpg<=2 (2026-08-21
 * fpg fix; `dw_raster_layer.cpp`'s active `dwr_consume` path hard-bounds
 * every fpg-indexed array/loop at `DWR_MAX_FPG=2` regardless of runtime
 * fpg -- confirmed 2026-08-29 to SIGSEGV at fpg=48, a synthetic stress
 * shape no real layer uses), K<=MAX_K=7 (2026-08-21 MAX_K 3->7 fix, 13 of
 * 52 real layers use K=7), and the raster DW mechanism's minimum cin /
 * exact-tile-multiple spatial dims (2026-08-30 -- cin=8 with otherwise-
 * safe 4-multiple dims fails silently, cin=48 with non-4-multiple dims
 * crashes outright, confirmed as two INDEPENDENT triggers, not either/
 * or; see CLAUDE.md's "known limitation" entry for the full isolation).
 * Consolidated here instead of left scattered across per-file comments
 * discovered after the fact, so the next person changing model or
 * resolution sees every known boundary in one place, not five.
 *
 * `mac_check_supported_shape()` fires these as plain `assert()`, guarded
 * by `#ifndef __SYNTHESIS__` (the standard Xilinx idiom -- Vitis HLS
 * defines `__SYNTHESIS__` only during csynth_design, so this code is
 * PRESENT and firing in csim, and ABSENT from synthesized RTL: zero real
 * hardware cost, and it cannot affect any deployed bitstream). The goal
 * is a LOUD, immediate csim failure if a future model/resolution change
 * ever crosses one of these lines -- this project has been burned
 * repeatedly by the alternative (silently wrong results that only show
 * up on real hardware, or not at all, since csim wasn't run against the
 * shape that mattered -- see CLAUDE.md's "check what a claimed
 * validation actually consumed" and "runtime value gating a hardware
 * region" entries).
 *
 * DW_CIN_MIN_SAFE=48 is an EMPIRICAL lower bound, not a theoretical one
 * (the user's own framing: measured floor, not derived from first
 * principles) -- cin=8 (with otherwise-safe 4-multiple spatial dims) was
 * directly confirmed to fail; cin=48 was directly confirmed to pass (the
 * wiring tb's own real-layer cases, and every real network DW layer).
 * The true minimum was not narrowed further between 8 and 48 -- do not
 * lower this constant without new evidence from an actual csim run at
 * the proposed lower value. */
#define MAC_SUPPORTED_FPG_MAX  2
#define DW_CIN_MIN_SAFE        48  /* empirical, not theoretical -- see above */

static inline void mac_check_supported_shape(const LayerDescV2 &d) {
#ifndef __SYNTHESIS__
    if (d.op_type == LDESC_OP_DWCONV) {
        assert(d.fpg <= MAC_SUPPORTED_FPG_MAX &&
               "DW fpg exceeds MAC_SUPPORTED_FPG_MAX=2 -- dw_raster_layer.cpp's dwr_consume hard-"
               "bounds every fpg-indexed array/loop at DWR_MAX_FPG=2 regardless of runtime fpg "
               "(ZHR-92 2026-08-29, SIGSEGV confirmed at fpg=48; no real layer needs fpg>2).");
        assert(d.k <= MAX_K &&
               "DW k exceeds MAX_K -- PATCH_R_MAX/PATCH_C_MAX and dw_wtile's per-channel storage "
               "are sized from MAX_K, not the real per-layer K.");
        assert(d.stride <= MAX_STRIDE &&
               "DW stride exceeds MAX_STRIDE -- PATCH_R_MAX/PATCH_C_MAX sizing assumes this bound.");
        assert(d.cin >= DW_CIN_MIN_SAFE &&
               "DW cin is below the empirically-confirmed-safe minimum (48) -- the raster DW "
               "mechanism was confirmed to silently produce near-all-zero output below this "
               "(ZHR-92 2026-08-30: cin=8 with safe spatial dims still failed, 1621/4096 "
               "mismatches). This does not affect the real network (every real DW layer has "
               "cin>=48) -- only a future model/resolution change could trip this.");
        assert((d.h_in % MAC_PR == 0) && (d.w_in % MAC_PC == 0) &&
               "DW h_in/w_in is not an exact multiple of MAC_PR/MAC_PC -- the raster DW mechanism "
               "was confirmed to CRASH outright on non-multiple spatial dims, even at safe cin "
               "(ZHR-92 2026-08-30: cin=48/h_in=w_in=15 crashed with an unknown-error abort). "
               "Every real DW layer's h_in/w_in is an exact multiple of 4, so this does not "
               "affect the real network -- only a future model/resolution change could trip it.");
    }
    if (d.op_type == LDESC_OP_PWCONV) {
        /* run_layer's ROW_READ_FILL loop (PW's row-hoist fast path) is
         * bounded by the compile-time MAX_WORDS_PER_CH=17, but the real
         * word count it needs is a RUNTIME value ((r+w_in+3)>>2, worst
         * case r=3) -- currently exactly tight against the real network's
         * worst case (w_in=64 -> 17 words exactly, verified against every
         * real PWCONV layer's w_in in tools/layer_descriptor_256.json,
         * zero margin). A w_in this bound doesn't cover would silently
         * truncate the row read, not crash -- the same failure shape as
         * the DW cin/dims bug above, just not yet independently exercised
         * by a synthetic test the way DW's was (flagged from the
         * derivation alone, not confirmed via a failing csim run). */
        assert(((3 + d.w_in + 3) >> 2) <= MAX_WORDS_PER_CH &&
               "PW w_in requires more words per row than MAX_WORDS_PER_CH=17 covers -- "
               "ROW_READ_FILL would silently truncate the row read (derivation-only, ZHR-92 "
               "2026-08-30; every real PWCONV layer's w_in<=64 stays exactly at this bound).");
    }
#endif
}

/* Host-side utility (stands in for the real descriptor generator). NOT
 * called from mac_array_top / not part of the synthesized design.
 * tools/verify_mac_array_mapping.py independently re-derives the same
 * arithmetic from scratch and diffs against what this function (via the
 * testbench's dump) actually produced. */
struct MacArrayParams {
    int h_out, w_out;
    int n_row_tiles, n_col_tiles, n_ch_tiles;
    int last_row_tile, last_col_tile, last_ch_tile;
    int in_ch_stride, out_ch_stride;  /* A3 (2026-08-21, ZHR-92): h_in*w_in,
                                        * h_out*w_out precomputed host-side.
                                        * Address arithmetic in run_layer
                                        * used to form these products with
                                        * a runtime multiply INSIDE the
                                        * per-lane loops -- confirmed by
                                        * direct diff of csynth Instance
                                        * tables (pre/post real m_axi
                                        * interface: run_layer's own DSP
                                        * count 71->106, +35, matching the
                                        * overall +41 DSP delta together
                                        * with mac_array_top's own +6).
                                        * Precomputing means hardware reads
                                        * a field instead of multiplying --
                                        * same technique round 3 used to
                                        * remove a hardware divider. */
};
MacArrayParams derive_mac_array_params(const LayerDescV2 &d);

/* Layer controller + MAC array top function. Executes ONE descriptor per
 * call -- ZHR-92 (2026-08-29): desc/n_layers/out_written[] moved off the
 * gmem_meta m_axi master onto s_axilite (single by-value desc, scalar
 * out_written), eliminating gmem_meta entirely. Every real dispatch was
 * already n_layers=1 (confirmed via source read of the board test
 * harnesses before this change), so this matches real hardware usage
 * exactly; a caller that wants to run N layers back-to-back now calls this
 * function N times, once per descriptor (mac_array_tb.cpp's multi-layer
 * phases were converted to this shape). out_written is set to 1 once (and
 * only once) this function has performed the real output write --
 * mac_array_tb.cpp's Phase 2 overrides the DRAM contents afterward (not
 * this function) to reproduce the Add defect's symptom (IP completes,
 * output silently not written) so the testbench's independent
 * verification step can be proven to catch it. */
void mac_array_top(
    LayerDescV2 desc,
    const act_t  in_base[],
    const wt_t   w_base[],
    const acc_t  b_base[],
    act_t        out_base[],
    int          *out_written,
    const ap_uint<32> in_base_wide[],
    hls::burst_maxi<ap_uint<32> > out_burst,
    hls::burst_maxi<ap_uint<32> > in_burst,
    hls::burst_maxi<ap_uint<32> > elemwise_in_burst,
    hls::burst_maxi<ap_uint<32> > elemwise_out_burst,
    hls::burst_maxi<ap_uint<32> > dw_in_burst
);

#endif // __MAC_ARRAY_H__
