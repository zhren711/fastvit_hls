/*============================================================
 * dsconv_worker.cpp -- fused depthwise(7x7)+pointwise(1x1) kernel,
 * on-chip patch-level DW->PW handoff (isolated feasibility study, see
 * plan C:\Users\zhren\.claude\plans\peppy-gathering-ripple.md).
 *
 * MOTIVATION: this project's own combined-design re-integration
 * (2026-08-11, dwconv line-buffer rewrite) found dwconv was never the
 * combined design's timing bottleneck -- pwconv's weight-loading FSM
 * was. Independently, the paper "ActiveSight" (PatchDW dataflow) fuses
 * depthwise+pointwise at the on-chip patch level specifically to
 * eliminate the DRAM round-trip between them. This kernel borrows that
 * idea for the ONE place in the real FastVIT-T8 network where it
 * applies cleanly: each of the 10 RepMixer blocks' trailing DW7 -> PW1
 * (expand) link, which is always stride=1/fpg=1/act_mode=ACT_NONE on
 * both sides with zero intervening ops (see plan for the driver trace
 * that established this -- Stem/Transition blocks have GELU between DW
 * and PW, breaking direct fusion; FinalDW has no downstream PW at all).
 *
 * HARD CORRECTNESS CONSTRAINT: the DW output ("token_mix" in the
 * driver's naming) is dual-consumed -- PW1 reads it immediately here,
 * AND the block's residual Add reads it again, unmodified, much later.
 * This kernel CANNOT skip materializing a full DRAM-resident copy of
 * the DW output (dw_feat_out below) even though it also feeds PW1
 * on-chip -- the real win is eliminating PW's read-BACK of it, not
 * both round-trips. dw_feat_out must be written EXACTLY ONCE per
 * pixel; see the WRITEBACK_DW_PATCH loop below, which is structurally
 * guaranteed to run exactly once per (patch,channel) by construction
 * (no outer loop wraps around it) -- do not "optimize" this by moving
 * it inside the PW tm-loop, that reintroduces the exact class of bug
 * pwconv_worker.cpp's header comments already document twice (weights/
 * accumulators recomputed or reused at the wrong loop level).
 *
 * ARCHITECTURE -- why patch tiling, not a naive merge:
 * dwconv_worker.cpp (this project's current line-buffer design) is
 * channel-SERIAL (one full channel's raster at a time) and spatially-
 * unrolled (KxK taps/cycle). pwconv_worker.cpp needs ALL CHin channels'
 * values at a given spatial position simultaneously (it's a channel-
 * reduction dot product) and is channel-tile-unrolled (PW_TM x PW_TN
 * MACs/cycle) with a spatially-tiled, weight-stationary GEMM structure
 * whose pw_out_buf spans the WHOLE feature map so PW's weights stay
 * resident across an entire layer. Naively nesting DW inside PW's tm
 * loop would recompute DW up to Tm_loops times (up to 18x for this
 * network's shapes) -- erasing the fusion's benefit. Naively letting
 * PW's accumulator span the whole image (as pwconv_worker.cpp does)
 * needs up to 2.25MB of BRAM for this network's largest block, ~3.5x
 * this chip's budget. The resolution: bound BOTH by a spatial PATCH
 * (DS_PATCH_MAX x DS_PATCH_MAX, see dsconv_worker.h) -- DW is computed
 * exactly once per (patch,channel) and cached on-chip in dw_patch_buf
 * for the WHOLE CHin range before any PW output-channel tile (tm)
 * consumes it. Phase A below is literally dwconv_worker.cpp's existing
 * per-channel line-buffer/window engine, just scoped to a patch's
 * haloed row/col range instead of the whole image. Phase B is
 * literally pwconv_worker.cpp's existing COMPUTE_PW_S/TM/TN inner
 * structure, unchanged, except it reads dw_patch_buf (on-chip BRAM)
 * instead of DMA'ing a fresh tile from DRAM via LOAD_PW_IN -- pw_in_buf
 * disappears entirely in this fused design.
 *
 * WORD-ALIGNMENT (why no read-modify-write is needed on the DRAM
 * writebacks): DS_PATCH_MAX=16 and this network's real H=W (64/32/16/8)
 * always divide evenly, so pr=pc is always 16 or 8 -- both multiples of
 * 8 -- and every patch's column start pc0 (a multiple of pc) is
 * therefore always a multiple of 8 too. That means every patch-row
 * segment (pc pixels wide, nibble-packed 8/word) starts and ends on a
 * word boundary in the GLOBAL (Hout x Wout) DRAM layout, so both
 * WRITEBACK_DW_PATCH and WRITEBACK_PW can write whole words directly,
 * no partial-word read-modify-write. This is a real constraint of this
 * design (not just an optimization), guarded implicitly by DS_PATCH_MAX
 * and the real network's power-of-2 spatial dims -- do not shrink
 * DS_PATCH_MAX below 8 or use it on a non-power-of-2-dimensioned layer
 * without revisiting this.
 *
 * SCOPE, deliberately bounded for this first milestone (see plan):
 * channel-chunk parallelism (pd>1, unrolling DW across multiple
 * channels at once) is NOT implemented -- Phase A processes one
 * channel at a time (pd=1), identical to today's dwconv_worker. DW
 * weights are reloaded once per (patch,channel) rather than cached
 * across patches for a given channel (a legitimate v2 optimization,
 * not solved here -- see the plan's DRAM-traffic accounting). Input
 * pixel reads in Phase A are NOT lane-cached across the 8-pixel packing
 * boundary the way dwconv_worker.cpp's sequential full-image scan does
 * (patches don't start word-aligned in general, only patch ROW STARTS
 * happen to be, per the word-alignment note above, so re-deriving
 * word_idx/lane per pixel is the simple-and-correct choice for this
 * milestone, not the bandwidth-optimal one).
 *============================================================*/

#include "dsconv_worker.h"

/* Narrow type for patch-local scan-position bookkeeping (row/col
 * offsets within a patch, halo extents) -- same "narrow local copy of a
 * wide runtime parameter" technique validated twice already in this
 * project (dwconv_worker.cpp's pos_t). Range needed here is under
 * ~DS_PATCH_MAX+DW_MAX_K (~23); 12 bits is generous headroom. */
typedef ap_int<12> pos_t;

/* Separate, wider type for the flat pixel-index arithmetic
 * (ih*Win+iw), which spans the full image (up to DW_MAX_H*DW_MAX_W=
 * 4096) -- deliberately NOT reusing pos_t here, since that range is a
 * genuinely different (larger) requirement than patch-local position
 * bookkeeping, and conflating the two would silently overflow pos_t. */
typedef ap_int<16> idx_t;

/* Narrow type for Phase B's (PW1) small-range loop counters/valid-
 * counts (tm,tn,m,n,s,lr,sw and tm_valid/tn_valid) -- max real range is
 * spatial_patch<=256 (DS_PATCH_MAX^2) and Tm_loops<=144 (CHout=1152/8),
 * both comfortably under 12 bits. Round-1 isolated P&R (see plan) found
 * a suspiciously wide (90-bit!) comparator on WRITEBACK_PW's flattened
 * loop indvar -- these counters were plain `int` (32-bit) despite their
 * tiny real range, the same class of problem pos_t already fixed twice
 * on the DW side of this project. Deliberately a SEPARATE typedef from
 * pos_t (not reused) to keep DW-patch-position and PW-tile-counter
 * bookkeeping semantically distinct, even though the underlying width
 * happens to match. */
typedef ap_int<12> tile_t;

/* dw_patch_buf: DW's per-(patch,channel) output cache, read by Phase B
 * instead of DMA'ing PW's activation tile from DRAM. Partition
 * cyclic factor=8 on the channel dim to match PW_TN=8 -- Phase B's
 * unrolled n=0..7 lane reads (cin_base+n, cin_base a multiple of 8)
 * land on 8 DIFFERENT banks at the SAME sub-address each cycle, giving
 * single-cycle parallel access across the whole channel-tile.
 * Leading PATCH_GROUP dim (see dsconv_worker.h's PATCH_GROUP comment):
 * holds PATCH_GROUP patches' DW output simultaneously so Phase B's
 * LOAD_PW_WT can be hoisted to run once per GROUP instead of once per
 * patch -- not partitioned (only one g is ever live in any given
 * pipelined region at a time, no simultaneous-g access needed). */
static act_t dw_patch_buf[PATCH_GROUP][DW_MAX_CH][DS_PATCH_MAX * DS_PATCH_MAX];
/* Partitioning pragmas for this file-scope static live inside
 * dsconv_worker() below (HLS requires function scope for pragmas),
 * same placement convention dwconv_worker.cpp uses for its own
 * file-scope ch_out_buf. */

static inline act_t dsconv_apply_act(acc_t val, int act_mode, int shift) {
#pragma HLS INLINE
    acc_t s = val >> shift;
    act_t r;  /* W8A4: clamp to symmetric 4-bit range -7..7. */
    if      (s >  7) r =  7;
    else if (s < -7) r = -7;
    else             r = (act_t)s;
    if (act_mode == ACT_RELU && r < 0) r = 0;
    return r;
}

void dsconv_worker(
    pack_t dw_feat_in[],
    wt_t   dw_weight[],
    acc_t  dw_bias[],
    pack_t dw_feat_out[],
    wt_t   pw_weight[],
    acc_t  pw_bias[],
    pack_t pw_feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    pad_h,
    int    pad_w,
    int    CHout,
    int    dw_act_mode,
    int    dw_out_shift,
    int    pw_act_mode,
    int    pw_out_shift)
{
#pragma HLS INTERFACE m_axi port=dw_feat_in  offset=slave bundle=gmem0 depth=262144 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
#pragma HLS INTERFACE m_axi port=dw_weight   offset=slave bundle=gmem1 depth=25088 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
/* pw_weight split onto its own bundle (gmem4) -- round-1 isolated P&R
 * (see plan) found the WORST path was exactly dw_weight/pw_weight
 * sharing gmem1: LOAD_PW_WT's FSM state driving gmem1's AXI
 * read-request-FIFO address SRL chain, WNS=-1.633ns. This is the same
 * "shared m_axi bundle -> AXI-adapter-internal fanout" bottleneck
 * class this whole project has repeatedly found (op_code fanout in the
 * combined design, etc.), just showing up here from sharing ONE
 * bundle between two genuinely-simultaneously-live arrays within a
 * single call -- unlike bias's cross-op_code sharing elsewhere in this
 * project, dw_weight and pw_weight are both actively read within the
 * same patch iteration, so contention is real, not just theoretical.
 * This deliberately steps outside the project's usual "stay at 4
 * masters" rule for the COMBINED design -- that rule is about the
 * 5-worker fastvit_ip top, not this isolated feasibility study; a 5th
 * bundle here is a data-driven experiment, not scope creep. */
#pragma HLS INTERFACE m_axi port=pw_weight   offset=slave bundle=gmem4 depth=786432 \
    latency=64 num_read_outstanding=4  max_read_burst_length=256
#pragma HLS INTERFACE m_axi port=dw_bias     offset=slave bundle=gmem2 depth=512
#pragma HLS INTERFACE m_axi port=pw_bias     offset=slave bundle=gmem2 depth=1536
#pragma HLS INTERFACE m_axi port=dw_feat_out offset=slave bundle=gmem3 depth=262144 \
    latency=64 num_write_outstanding=4 max_write_burst_length=256
#pragma HLS INTERFACE m_axi port=pw_feat_out offset=slave bundle=gmem3 depth=786432 \
    latency=64 num_write_outstanding=4 max_write_burst_length=256

#pragma HLS INTERFACE s_axilite port=CHin        bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Hin         bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Win         bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kh          bundle=ctrl
#pragma HLS INTERFACE s_axilite port=Kw          bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_h       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pad_w       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=CHout       bundle=ctrl
#pragma HLS INTERFACE s_axilite port=dw_act_mode bundle=ctrl
#pragma HLS INTERFACE s_axilite port=dw_out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pw_act_mode bundle=ctrl
#pragma HLS INTERFACE s_axilite port=pw_out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return      bundle=ctrl

#pragma HLS ARRAY_PARTITION variable=dw_patch_buf cyclic factor=8 dim=2
#pragma HLS BIND_STORAGE variable=dw_patch_buf type=RAM_1P impl=BRAM

    /* line_buf/window: identical structure to
     * dwconv_worker.cpp, just patch-width instead of full-image-width
     * (DS_PATCH_MAX+DW_MAX_K-1 instead of DW_MAX_W) -- each (patch,
     * channel) scan is self-contained and self-flushing exactly like
     * the original (see dwconv_worker.cpp design note 3): the window
     * only becomes valid, and stale content only becomes readable,
     * after DW_MAX_K-1 rows of THIS scan have already overwritten it. */
    static act_t line_buf[DW_MAX_K - 1][DS_PATCH_MAX + DW_MAX_K - 1];
#pragma HLS ARRAY_PARTITION variable=line_buf complete dim=1
    act_t window[DW_MAX_K][DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=window complete dim=0
    /* dw_wt_buf removed in round-3: dw_wt_cache (below) already holds
     * every channel's FLIPPED weights, loaded once before the patch
     * loop -- the dot product reads dw_wt_cache[ch][i][j] directly, no
     * separate per-(patch,channel) working copy needed. */

    /* pw_wt_buf/pw_acc: same shapes/partitioning pwconv_worker.cpp
     * uses for its own pw_wt_buf and (the CHout dim of) pw_out_buf --
     * the only difference is pw_acc's spatial dim is patch-bounded
     * (DS_PATCH_MAX^2) instead of whole-image-bounded
     * (PW_MAX_SPATIAL) -- this IS the fusion's memory-footprint win. */
    wt_t pw_wt_buf[PW_TM][PW_TN];
#pragma HLS ARRAY_PARTITION variable=pw_wt_buf complete dim=1
#pragma HLS ARRAY_PARTITION variable=pw_wt_buf complete dim=2
    /* Leading PATCH_GROUP dim, same rationale as dw_patch_buf above --
     * one weight tile (pw_wt_buf) now gets reused across all
     * group_size patches' accumulators before the next tile loads.
     * Not partitioned on the group dim (only one g processed at a time
     * in COMPUTE_GROUP/WRITEBACK_PW below); PW_TM dim (dim=2) keeps its
     * original complete partition since COMPUTE_PW_TM still unrolls all
     * 8 m-lanes simultaneously for whichever g is currently live. */
    acc_t pw_acc[PATCH_GROUP][PW_TM][DS_PATCH_MAX * DS_PATCH_MAX];
#pragma HLS ARRAY_PARTITION variable=pw_acc complete dim=2
    /* Round-6 (reverted): tried #pragma HLS ARRAY_PARTITION variable=pw_acc
     * cyclic factor=8 dim=3, targeting round-5's #1 worst path
     * (pw_acc_15_U/ram_reg -> WRITEBACK_PW's output-packing register,
     * route-delay-dominated per timing_detail_..._round5.rpt). csim
     * passed, but the SAME csynth run showed HLS's own scheduler could
     * NOT keep COMPUTE_GROUP_COMPUTE_PW_S (the MAC accumulation hot
     * loop, pw_acc[g][m][s] += dot at line ~604) at II=1 anymore --
     * "Unable to schedule 'load' operation on array 'pw_acc_6' due to
     * limited memory ports" / "Target II = 1, Final II = 2" -- doubling
     * that loop's cycle count. Root cause: cyclically banking pw_acc's
     * spatial dim to give WRITEBACK_PW's 8-wide read cheap per-bank
     * access collided with COMPUTE_GROUP's independent per-cycle
     * read-modify-write at an arbitrary runtime address s, which the
     * unpartitioned array satisfied via a single BRAM port's inherent
     * read-before-write same-address-same-cycle behavior -- something
     * the partitioned/banked form couldn't replicate. Caught via HLS's
     * own II-violation warning before spending a full P&R run on it;
     * reverted. Any future fix for WRITEBACK_PW's route-delay-dominated
     * path needs to NOT touch pw_acc's dim=3 storage layout, since
     * COMPUTE_GROUP's accumulate pattern owns that array too. */
#pragma HLS BIND_STORAGE variable=pw_acc type=RAM_1P impl=BRAM

    /* Round-3 weight caches (see header comment on PW_WT_CACHE_MAX):
     * loaded ONCE per call, before the patch loop, instead of being
     * DMA-reread once per patch -- this is what fixes the large-
     * Npatches ("large resolution") blocks' DRAM-traffic net loss
     * found by round-2's measurement. dw_wt_cache is small (~25KB)
     * and always used unconditionally. pw_wt_cache is only used when
     * the real CHout*CHin fits; otherwise Phase B falls back to the
     * original per-(patch,tm,tn) DMA reload path (harmless for
     * Npatches=1 shapes, where "per patch" already equals "once"). */
    /* Round-4: ARRAY_RESHAPE (not ARRAY_PARTITION) on both kernel dims --
     * LOOP_DOT_I/LOOP_DOT_J below still needs all 49 (i,j) taps readable
     * in the same cycle (same ch/address), but ARRAY_PARTITION-per-dim
     * forced one dedicated BRAM tile per tap regardless of how little
     * each held: 512-deep x 8-bit = 4096 of an 18432-bit RAMB18E1 (22%
     * packed), and round-3's isolated P&R (see plan) confirmed via
     * hierarchical utilization diff that these 49 near-empty tiles were
     * the ENTIRE source of round-3's BRAM growth (round2->round3:
     * RAMB18E1 130->179, exactly +49) despite neither cache sitting
     * directly on the WRITEBACK_PW critical path -- i.e. the fragmented
     * tile count itself was degrading placement/routing quality
     * elsewhere on the die. ARRAY_RESHAPE keeps the identical
     * dw_wt_cache[ch][i][j] access pattern and simultaneous-read
     * capability (so LOOP_DOT_I/J's II=1 unroll is unaffected) but packs
     * all 49 taps into one wide word per depth-512 row instead of 49
     * separate primitives, letting the tool actually fill each tile. */
    static wt_t dw_wt_cache[DW_MAX_CH][DW_MAX_K][DW_MAX_K];
#pragma HLS ARRAY_RESHAPE variable=dw_wt_cache complete dim=2
#pragma HLS ARRAY_RESHAPE variable=dw_wt_cache complete dim=3
#pragma HLS BIND_STORAGE variable=dw_wt_cache type=RAM_1P impl=BRAM
    static wt_t pw_wt_cache[PW_WT_CACHE_MAX];
#pragma HLS ARRAY_PARTITION variable=pw_wt_cache cyclic factor=8 dim=1
#pragma HLS BIND_STORAGE variable=pw_wt_cache type=RAM_1P impl=BRAM

    int Hout = Hin + 2 * pad_h - Kh + 1;   /* stride=1 always for this link */
    int Wout = Win + 2 * pad_w - Kw + 1;
    int hw_in_words  = (Hin  * Win)  >> 3;
    int hw_out_words = (Hout * Wout) >> 3; /* per-channel word count, DW side */

    int pr = (Hout < DS_PATCH_MAX) ? Hout : DS_PATCH_MAX;
    int pc = (Wout < DS_PATCH_MAX) ? Wout : DS_PATCH_MAX;
    int patches_r = Hout / pr;  /* real FastVIT-T8 shapes divide evenly; see header note */
    int patches_c = Wout / pc;

    pos_t Kh_n     = Kh;
    pos_t Kw_n     = Kw;
    pos_t pad_h_n  = pad_h;
    pos_t pad_w_n  = pad_w;
    pos_t Hin_n    = Hin;
    pos_t Win_n    = Win;
    pos_t pr_n     = pr;
    pos_t pc_n     = pc;
    /* Narrow copies for Phase B / WRITEBACK_DW_PATCH loop bounds -- see
     * tile_t's comment above. */
    tile_t pr_t       = pr;
    tile_t pc_words_t = pc / 8;

    /* ---- One-time weight cache fills (before the patch loop) ---- */
    LOAD_DW_WT_CACHE:
    for (int ch = 0; ch < CHin; ch++) {
#pragma HLS LOOP_TRIPCOUNT min=48 max=384
        for (int kh = 0; kh < Kh; kh++) {
#pragma HLS PIPELINE II=1
            for (int kw = 0; kw < Kw; kw++) {
                dw_wt_cache[ch][Kh - 1 - kh][Kw - 1 - kw] =
                    dw_weight[ch * Kh * Kw + kh * Kw + kw];
            }
        }
    }

    bool use_pw_cache = (CHout * CHin <= PW_WT_CACHE_MAX);
    if (use_pw_cache) {
        LOAD_PW_WT_CACHE:
        for (int i = 0; i < CHout * CHin; i++) {
#pragma HLS PIPELINE II=1
            pw_wt_cache[i] = pw_weight[i];
        }
    }

    /* Patches are flattened into a single index (pr_idx = patch_idx /
     * patches_c, pc_idx = patch_idx % patches_c) so grouping PATCH_GROUP
     * consecutive patches together doesn't need a 2D group shape --
     * see dsconv_worker.h's PATCH_GROUP comment for why grouping exists
     * at all. */
    int total_patches = patches_r * patches_c;

    LOOP_PATCH_GROUP:
    for (int group_base = 0; group_base < total_patches; group_base += PATCH_GROUP) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=8
        tile_t group_size = (tile_t)((total_patches - group_base < PATCH_GROUP) ?
            (total_patches - group_base) : PATCH_GROUP);

        pos_t g_pr0[PATCH_GROUP];
        pos_t g_pc0[PATCH_GROUP];
#pragma HLS ARRAY_PARTITION variable=g_pr0 complete dim=1
#pragma HLS ARRAY_PARTITION variable=g_pc0 complete dim=1
        SETUP_GROUP_ORIGINS:
        for (tile_t g = 0; g < group_size; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=2
            int patch_idx = group_base + (int)g;
            int pr_idx = patch_idx / patches_c;
            int pc_idx = patch_idx % patches_c;
            g_pr0[(int)g] = pr_idx * pr;   /* patch's global output row/col origin */
            g_pc0[(int)g] = pc_idx * pc;
        }

        /* ================= Phase A (grouped): DW7 for every patch in this group ================= */
        LOOP_GROUP_A:
        for (tile_t g = 0; g < group_size; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=2
        pos_t pr0 = g_pr0[(int)g];
        pos_t pc0 = g_pc0[(int)g];

            LOOP_DS_CH:
            for (int ch = 0; ch < CHin; ch++) {
#pragma HLS LOOP_TRIPCOUNT min=48 max=384

                /* Weights already cached (flipped) in dw_wt_cache[ch] by
                 * LOAD_DW_WT_CACHE, once, before the patch loop -- see
                 * round-3 comment above. No per-(patch,channel) DMA
                 * reload here anymore. */
                acc_t dw_bias_val = dw_bias[ch];

                LOOP_DS_ROW:
                for (pos_t eih = 0; eih < pr_n + Kh_n - 1; eih++) {
#pragma HLS LOOP_TRIPCOUNT min=8 max=22
                    pos_t ih = pr0 - pad_h_n + eih;
                    bool row_real    = (ih >= 0 && ih < Hin_n);
                    bool row_trigger = (eih >= Kh_n - 1);
                    pos_t oh_local   = row_trigger ? (pos_t)(eih - (Kh_n - 1)) : (pos_t)0;

                    LOOP_DS_COL:
                    for (pos_t eiw = 0; eiw < pc_n + Kw_n - 1; eiw++) {
#pragma HLS PIPELINE II=1
#pragma HLS LOOP_TRIPCOUNT min=8 max=22
                        pos_t iw = pc0 - pad_w_n + eiw;
                        bool col_real = (iw >= 0 && iw < Win_n);

                        act_t pixel = 0;
                        if (row_real && col_real) {
                            idx_t flat     = (idx_t)((int)ih * Win + (int)iw);
                            idx_t word_idx = flat >> 3;
                            ap_uint<3> lane = flat.range(2, 0);
                            pack_t word = dw_feat_in[ch * hw_in_words + (int)word_idx];
                            act_t lanes8[8];
#pragma HLS ARRAY_PARTITION variable=lanes8 complete dim=1
                            lanes8[0] = (act_t)word.range( 3,  0);
                            lanes8[1] = (act_t)word.range( 7,  4);
                            lanes8[2] = (act_t)word.range(11,  8);
                            lanes8[3] = (act_t)word.range(15, 12);
                            lanes8[4] = (act_t)word.range(19, 16);
                            lanes8[5] = (act_t)word.range(23, 20);
                            lanes8[6] = (act_t)word.range(27, 24);
                            lanes8[7] = (act_t)word.range(31, 28);
                            pixel = lanes8[lane];
                        }

                        act_t new_col[DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=new_col complete dim=1
                        new_col[0] = pixel;
                        LOOP_ROWHIST:
                        for (int i = 1; i < DW_MAX_K; i++) {
#pragma HLS UNROLL
                            new_col[i] = col_real ? line_buf[i - 1][eiw] : (act_t)0;
                        }

                        LOOP_WIN_ROW:
                        for (int i = 0; i < DW_MAX_K; i++) {
#pragma HLS UNROLL
                            LOOP_WIN_COL:
                            for (int j = DW_MAX_K - 1; j > 0; j--) {
#pragma HLS UNROLL
                                window[i][j] = window[i][j - 1];
                            }
                            window[i][0] = new_col[i];
                        }

                        if (col_real) {
                            LOOP_LINEBUF_SHIFT:
                            for (int i = DW_MAX_K - 2; i > 0; i--) {
#pragma HLS UNROLL
                                line_buf[i][eiw] = line_buf[i - 1][eiw];
                            }
                            line_buf[0][eiw] = pixel;
                        }

                        pos_t col_rel     = eiw - (Kw_n - 1);
                        bool  col_trigger = (col_rel >= 0);
                        pos_t ow_local    = col_trigger ? col_rel : (pos_t)0;

                        if (row_trigger && col_trigger) {
                            acc_t sum = dw_bias_val;
                            LOOP_DOT_I:
                            for (int i = 0; i < DW_MAX_K; i++) {
#pragma HLS UNROLL
                                LOOP_DOT_J:
                                for (int j = 0; j < DW_MAX_K; j++) {
#pragma HLS UNROLL
                                    if (i < Kh && j < Kw) {
                                        sum += (acc_t)window[i][j] * (acc_t)dw_wt_cache[ch][i][j];
                                    }
                                }
                            }
                            act_t dw_val = dsconv_apply_act(sum, dw_act_mode, dw_out_shift);
                            /* pc is a runtime int here (16 or 8); local_pixel fits well
                             * within DS_PATCH_MAX*DS_PATCH_MAX=256. */
                            dw_patch_buf[(int)g][ch][(int)oh_local * pc + (int)ow_local] = dw_val;
                        } // row_trigger && col_trigger
                    } // col
                } // row

                /* Materialize this channel's patch to DRAM for the later
                 * residual Add -- see header comment: EXACTLY ONCE per
                 * pixel, no outer loop wraps this. Word-aligned per the
                 * design note above (pc0, pc both multiples of 8 for all
                 * real shapes). */
                WRITEBACK_DW_PATCH:
                for (tile_t lr = 0; lr < pr_t; lr++) {
                    int oh = (int)pr0 + (int)lr;
                    int row_word_base = ch * hw_out_words + oh * (Wout >> 3) + ((int)pc0 >> 3);
                    /* Round-5: hoist the lr*pc runtime multiply out of the
                     * II=1 pipeline below -- lr is loop-invariant across
                     * sw, so leaving "lr*pc" textually inside the pipelined
                     * loop body forced it to be recomputed (and to drive
                     * dw_patch_buf's read address) every single cycle. This
                     * is the exact same address-arithmetic anti-pattern
                     * round-2/3/4 P&R diagnosed on WRITEBACK_PW below;
                     * WRITEBACK_DW_PATCH has the identical textual pattern
                     * so it's fixed here too, pre-emptively, before it
                     * becomes the next bottleneck once WRITEBACK_PW's is
                     * gone -- see this project's repeated "fix one path,
                     * next one surfaces" history. row_base is now computed
                     * ONCE per lr (a real multiply, but off the hot path);
                     * the pipelined loop only ever adds sw*8, a compile-time
                     * power-of-2 constant multiply (a free shift). */
                    int row_base = (int)lr * pc;
                    for (tile_t sw = 0; sw < pc_words_t; sw++) {
#pragma HLS PIPELINE II=1
                        pack_t word;
                        int base = row_base + (int)sw * 8;
                        word.range( 3,  0) = dw_patch_buf[(int)g][ch][base + 0];
                        word.range( 7,  4) = dw_patch_buf[(int)g][ch][base + 1];
                        word.range(11,  8) = dw_patch_buf[(int)g][ch][base + 2];
                        word.range(15, 12) = dw_patch_buf[(int)g][ch][base + 3];
                        word.range(19, 16) = dw_patch_buf[(int)g][ch][base + 4];
                        word.range(23, 20) = dw_patch_buf[(int)g][ch][base + 5];
                        word.range(27, 24) = dw_patch_buf[(int)g][ch][base + 6];
                        word.range(31, 28) = dw_patch_buf[(int)g][ch][base + 7];
                        dw_feat_out[row_word_base + sw] = word;
                    }
                }
            } // ch (Phase A)
        } // g (LOOP_GROUP_A)

        /* ================= Phase B (grouped): PW1, CHout-tile sweep, weight tile shared across the group ================= */
            /* Narrow (tile_t) copies of every small-range trip-count/valid-count
             * used as a loop bound below -- see tile_t's comment above. The wide
             * (int) row_base/row_word_base address arithmetic further down is
             * deliberately left wide (genuinely needs it), same "narrow the
             * bookkeeping, not the addressing" split this project's dwconv
             * rewrite already established. */
            tile_t Tm_loops = (CHout + PW_TM - 1) / PW_TM;
            tile_t Tn_loops = (CHin  + PW_TN - 1) / PW_TN;
            tile_t spatial_patch = pr * pc;
            int hw_out_words_pw = (Hout * Wout) >> 3; /* per-CHout-channel word count, PW side */

            LOOP_PW_TM:
            for (tile_t tm = 0; tm < Tm_loops; tm++) {
                int cout_base   = (int)tm * PW_TM;
                int cout_end    = (cout_base + PW_TM < CHout) ? (cout_base + PW_TM) : CHout;
                tile_t tm_valid = cout_end - cout_base;

                INIT_PW_ACC:
                for (tile_t g = 0; g < group_size; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=2
                    for (tile_t m = 0; m < PW_TM; m++) {
#pragma HLS UNROLL
                        acc_t b = (m < tm_valid) ? pw_bias[cout_base + (int)m] : (acc_t)0;
                        for (tile_t s = 0; s < spatial_patch; s++) {
#pragma HLS PIPELINE II=1
                            pw_acc[(int)g][(int)m][(int)s] = b;
                        }
                    }
                }

                LOOP_PW_TN:
                for (tile_t tn = 0; tn < Tn_loops; tn++) {
                    int cin_base    = (int)tn * PW_TN;
                    int cin_end     = (cin_base + PW_TN < CHin) ? (cin_base + PW_TN) : CHin;
                    tile_t tn_valid = cin_end - cin_base;

                    /* Grouped-patch weight reuse (see dsconv_worker.h's
                     * PATCH_GROUP comment): this weight-tile load now runs
                     * ONCE per (group,tm,tn) instead of once per
                     * (patch,tm,tn) -- shared by all group_size patches in
                     * COMPUTE_GROUP below. When use_pw_cache is false (the
                     * weight matrix doesn't fit pw_wt_cache -- see
                     * PW_WT_CACHE_MAX), this directly cuts pw_weight's
                     * redundant DRAM re-reads by up to PATCH_GROUP x for
                     * large-Npatches shapes that also have large CHout*CHin
                     * (a combination no real FastVIT-T8 shape hits today,
                     * see PATCH_GROUP comment -- this is future-proofing,
                     * verified so far only via csim on a synthetic shape
                     * built to exercise it). When use_pw_cache is true this
                     * just saves redundant BRAM-to-BRAM copies, not DRAM
                     * traffic. */
                    LOAD_PW_WT:
                    for (tile_t m = 0; m < PW_TM; m++) {
                        int row_base = (cout_base + (int)m) * CHin + cin_base;
#pragma HLS PIPELINE II=1
                        for (tile_t n = 0; n < PW_TN; n++) {
                            wt_t wv = use_pw_cache ? pw_wt_cache[row_base + (int)n]
                                                    : pw_weight[row_base + (int)n];
                            pw_wt_buf[(int)m][(int)n] = (m < tm_valid && n < tn_valid) ?
                                wv : (wt_t)0;
                        }
                    }

                    COMPUTE_GROUP:
                    for (tile_t g = 0; g < group_size; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=2
                        COMPUTE_PW_S:
                        for (tile_t s = 0; s < spatial_patch; s++) {
#pragma HLS PIPELINE II=1
                            COMPUTE_PW_TM:
                            for (tile_t m = 0; m < PW_TM; m++) {
#pragma HLS UNROLL
                                acc_t dot = 0;
                                COMPUTE_PW_TN:
                                for (tile_t n = 0; n < PW_TN; n++) {
#pragma HLS UNROLL
                                    act_t inval = (n < tn_valid) ? dw_patch_buf[(int)g][cin_base + (int)n][(int)s] : (act_t)0;
                                    dot += (acc_t)inval * (acc_t)pw_wt_buf[(int)m][(int)n];
                                }
                                pw_acc[(int)g][(int)m][(int)s] += dot;
                            }
                        }
                    } // g (COMPUTE_GROUP)
                } // tn -- all CHin now accumulated for this (group,tm)

                WRITEBACK_PW:
                for (tile_t g = 0; g < group_size; g++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=2
                    pos_t pr0 = g_pr0[(int)g];
                    pos_t pc0 = g_pc0[(int)g];
                    for (tile_t m = 0; m < tm_valid; m++) {
                        for (tile_t lr = 0; lr < pr_t; lr++) {
                            int oh = (int)pr0 + (int)lr;
                            int row_word_base = (cout_base + (int)m) * hw_out_words_pw + oh * (Wout >> 3) + ((int)pc0 >> 3);
                            /* Round-5: this is the exact address arithmetic
                             * round-2/3/4 P&R identified (via report_timing,
                             * confirmed unchanged across all 3 rounds) as this
                             * design's worst path -- "lr*pc" was textually
                             * inside the II=1-pipelined sw loop below even
                             * though lr is loop-invariant there, so the
                             * runtime multiply (pc is a runtime int, 8 or 16)
                             * was being re-evaluated every cycle and fed
                             * directly into pw_acc's BRAM ADDRARDADDR/
                             * ADDRBWRADDR ports (add_ln523/trunc_ln520 in the
                             * round-3/4 reports). Round-4 proved this is a
                             * genuine LOCAL logic-depth/fanout problem, not a
                             * BRAM-fragmentation side effect (cutting 19% of
                             * BRAM tiles left WNS completely unchanged). Fix:
                             * hoist the multiply to run ONCE per lr (a real
                             * multiply, but off the hot per-cycle pipeline);
                             * the pipelined loop now only adds sw*8, a
                             * compile-time power-of-2 multiply (free shift). */
                            int row_base = (int)lr * pc;
                            for (tile_t sw = 0; sw < pc_words_t; sw++) {
#pragma HLS PIPELINE II=1
                                pack_t word;
                                int base = row_base + (int)sw * 8;
                                word.range( 3,  0) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 0], pw_act_mode, pw_out_shift);
                                word.range( 7,  4) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 1], pw_act_mode, pw_out_shift);
                                word.range(11,  8) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 2], pw_act_mode, pw_out_shift);
                                word.range(15, 12) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 3], pw_act_mode, pw_out_shift);
                                word.range(19, 16) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 4], pw_act_mode, pw_out_shift);
                                word.range(23, 20) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 5], pw_act_mode, pw_out_shift);
                                word.range(27, 24) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 6], pw_act_mode, pw_out_shift);
                                word.range(31, 28) = dsconv_apply_act(pw_acc[(int)g][(int)m][base + 7], pw_act_mode, pw_out_shift);
                                pw_feat_out[row_word_base + (int)sw] = word;
                            }
                        }
                    }
                } // g (WRITEBACK_PW)
            } // tm (Phase B)

    } // group
}
