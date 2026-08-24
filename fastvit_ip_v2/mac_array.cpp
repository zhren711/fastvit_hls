#include "mac_array.h"

/* Host-side only (see mac_array.h) -- NOT reachable from mac_array_top's
 * call graph, so none of this division is synthesized into hardware.
 * ch_dim is ALWAYS Cin (round 5): DW's real per-channel parallel tiling
 * (cin==cout for depthwise) and PW's Cin reduction-chunk stepping both
 * key off the same arithmetic. */
MacArrayParams derive_mac_array_params(const LayerDescV2 &d)
{
    MacArrayParams p;
    p.h_out = (d.h_in + 2 * d.pad - d.k) / d.stride + 1;
    p.w_out = (d.w_in + 2 * d.pad - d.k) / d.stride + 1;
    int ch_dim = d.cin;

    p.n_row_tiles = (p.h_out + MAC_PR - 1) / MAC_PR;
    p.n_col_tiles = (p.w_out + MAC_PC - 1) / MAC_PC;
    p.n_ch_tiles  = (ch_dim  + MAC_PD - 1) / MAC_PD;

    p.last_row_tile = p.h_out - (p.n_row_tiles - 1) * MAC_PR;
    p.last_col_tile = p.w_out - (p.n_col_tiles - 1) * MAC_PC;
    p.last_ch_tile  = ch_dim  - (p.n_ch_tiles  - 1) * MAC_PD;

    p.in_ch_stride  = d.h_in * d.w_in;
    p.out_ch_stride = p.h_out * p.w_out;
    return p;
}

static acc_t clip_shift(acc_t acc, int shift)
{
    acc_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return v;
}

/* A3 round (2026-08-24, ZHR-92, run_layer rewrite stage 4): the old
 * run_reduce_dw (itself renamed from run_reduce_unified in stage 2, its
 * own history preserved in git log -- drive_mac's removal in round 3,
 * the round 9-14 shared-instance/fan-out lessons that shaped its
 * GATHER+UNIFIED split) is GONE. dw_one_ot_flat (below run_layer) folds
 * the same compute directly into its own flat pipeline instead of
 * calling out to a separate reduction function -- there is no longer
 * any call site left for a standalone DW reduction function at all.
 * The lessons that comment block documented (don't branch on op_type
 * inside a pipelined region; don't let a runtime-derived index feed an
 * unrolled read) still hold and are applied fresh in dw_one_ot_flat's
 * own header comment, not restated here. */

/* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2): PW's flat
 * pipeline. Replaces PW's entire per-(rt,colt) (ot,cbase) loop +
 * WRITEOUT_PW with ONE counter-driven `PIPELINE II=1` loop, called ONCE
 * per (rt,colt) tile (handling every ot/cbase internally) instead of
 * the previous per-ot/per-cbase nested-loop round-trips. DW is
 * completely untouched -- still calls run_reduce_unified via its own
 * per-ot loop in run_layer below, unchanged, so stage 3's P&R cleanly
 * isolates PW's own resource/timing delta.
 *
 * See the design doc + stage-1 spike (flat_pipeline_probe.cpp) on
 * ZHR-92 for the full rationale and the four stage-1 findings carried
 * forward here:
 *   - II=2 achieved in the spike (not the targeted 1) -- acc's
 *     cross-iteration dependency stalls at the compute<->writeout phase
 *     transition. Still a real, large win if it survives here: the
 *     spike predicted ~64 cycles for an isolated n_cbase=1 ot (vs. the
 *     measured 870 today) and ~1,184 cycles for a 36-cbase real layer
 *     (vs. 11,852 today) -- roughly 10-13x fewer cycles.
 *   - Zero divide/modulo confirmed in the spike -- every derived index
 *     here is a loop-carried accumulator with wrap-pair logic, never a
 *     computed index/mod (round 13's `kh = step / MAX_K` mistake,
 *     avoided by construction).
 *   - LUT net delta is an accepted uncertainty, not resolved by the
 *     spike (which structurally couldn't see the "hidden run_layer
 *     glue" cost this whole investigation has been chasing) -- real P&R
 *     is the stop-loss (current baseline 96%, ~1,800 LUT headroom).
 *   - Burst inference is explicitly OUT OF SCOPE this round (10x
 *     flattening win prioritized over burst's 2-3x, already failed
 *     three times on the old WRITEOUT_PW shape) -- WRITEOUT keeps its
 *     boundary guard for partial tiles, same as before; revisit burst
 *     separately later once this shape has settled, re-diagnosing from
 *     scratch since the structure has changed.
 *
 * Generalized beyond the stage-1 spike to handle what real layers
 * actually need:
 *   - Partial last cbase chunk (Cin not a multiple of MAX_CIN_PW=32):
 *     each cbase is still a compile-time-fixed PW_FLAT_STEPS_PER_CBASE
 *     steps (not a runtime bound) -- the weight read is zero-filled for
 *     any channel >= Cin, same established pattern as PW_WSTAGE's
 *     existing zero-fill (nulls the product regardless of what
 *     pw_patch_full holds past Cin, same channel-bounds safety argument
 *     already used elsewhere in this file -- c0+cib+dd's max value is
 *     always < MAX_CIN, so the read itself is always safe even when the
 *     data is stale).
 *   - Partial spatial tile (r_sz<MAC_PR or col_sz<MAC_PC, e.g. layer
 *     50/51's w_out=1): WRITEOUT keeps its own boundary guard, unchanged
 *     correctness-wise from WRITEOUT_PW's guarded form.
 *   - use_shift_table: shift is read once per ot, at the compute->
 *     writeout phase transition (not every cycle) -- same
 *     loop-invariant-hoist discipline as the reverted WRITEOUT
 *     dual-path round's surviving half. */
#define PW_FLAT_STEPS_PER_CBASE (MAX_CIN_PW / MAC_PD)
#define PW_FLAT_WRITEOUT_ELEMS  (MAC_PR * MAC_PC)

static void pw_flat_pipeline(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    int rt, int colt, int r_sz, int col_sz)
{
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic factor=MAC_PD dim=1
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
    #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3

    const int Cin = d.cin;
    const int n_ot = d.cout;
    const int n_cbase = (Cin + MAX_CIN_PW - 1) / MAX_CIN_PW;
    const int total_iters = n_ot * (n_cbase * PW_FLAT_STEPS_PER_CBASE + PW_FLAT_WRITEOUT_ELEMS);

    acc_t acc[MAC_PD][MAC_PR][MAC_PC];
    #pragma HLS ARRAY_PARTITION variable=acc complete dim=0

    /* loop-carried state -- all plain counters, wrap via compare+add,
     * no derived multiply/divide/mod anywhere in the hot loop (see the
     * spike's own header comment for why this matters). */
    int k = 0;                  /* 0..PW_FLAT_STEPS_PER_CBASE-1, dual-use */
    bool in_writeout = false;
    int cbase_idx = 0;
    int ch_off = 0;              /* channel offset within Cin, shared by patch+weight addressing */
    int w_ot_base = 0;           /* == ot*Cin, accumulated */
    int ot_out_ch_base = 0;      /* == ot*d.out_ch_stride, accumulated */
    int ot_idx = 0;
    int wr_row = 0, wr_col = 0;  /* writeout row/col -- wrap-pair, not idx/MAC_PC and idx%MAC_PC */
    int shift_reg = d.out_shift; /* recomputed at each ot's compute->writeout transition */

    PW_FLAT: for (int i = 0; i < total_iters; i++) {
        #pragma HLS PIPELINE II=1
        bool reset_acc = (!in_writeout) && (cbase_idx == 0) && (k == 0);

        if (!in_writeout) {
            wt_t lane_w[MAC_PD];
            act_t lane_in[MAC_PD][MAC_PR][MAC_PC];
            #pragma HLS ARRAY_PARTITION variable=lane_w complete dim=0
            #pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                bool ch_valid = (ch_off + dd) < Cin;
                lane_w[dd] = ch_valid ? w_base[d.w_off + w_ot_base + ch_off + dd] : (wt_t)0;
                for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        lane_in[dd][rr][cw] = pw_patch_full[ch_off + dd][rr][cw];
                    }
                }
            }
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        acc_t prod = (acc_t)lane_in[dd][rr][cw] * (acc_t)lane_w[dd];
                        acc[dd][rr][cw] = reset_acc ? prod : (acc_t)(acc[dd][rr][cw] + prod);
                    }
                }
            }
        } else {
            if (wr_row < r_sz && wr_col < col_sz) {
                acc_t total = 0;
                for (int dd = 0; dd < MAC_PD; dd++) {
                    #pragma HLS UNROLL
                    total += acc[dd][wr_row][wr_col];
                }
                total += pw_bias_cache[ot_idx];
                out_base[d.out_off + ot_out_ch_base + (rt * MAC_PR + wr_row) * d.w_out + (colt * MAC_PC + wr_col)] =
                    (act_t)clip_shift(total, shift_reg);
            }
        }

        /* counter update -- no division/modulo anywhere */
        if (k == PW_FLAT_STEPS_PER_CBASE - 1) {
            k = 0;
            if (!in_writeout) {
                if (cbase_idx == n_cbase - 1) {
                    in_writeout = true;
                    shift_reg = d.use_shift_table ? (int)w_base[d.shift_off + ot_idx] : d.out_shift;
                } else {
                    /* A3 round (2026-08-23, ZHR-92, run_layer rewrite
                     * stage 2 followup): FOUND AND FIXED via csim, not
                     * assumed correct -- ch_off is already sitting at
                     * cbase_idx*MAX_CIN_PW + (PW_FLAT_STEPS_PER_CBASE-1)*
                     * MAC_PD at this point (advanced by MAC_PD every
                     * non-wrap step above), which equals
                     * (cbase_idx+1)*MAX_CIN_PW - MAC_PD exactly, since
                     * PW_FLAT_STEPS_PER_CBASE*MAC_PD == MAX_CIN_PW by
                     * construction. One more +=MAC_PD (the SAME increment
                     * every other step uses) lands exactly on the next
                     * cbase's base -- += MAX_CIN_PW here overshot by a
                     * whole chunk (caught by csim: Phase12/13's real
                     * n_cbase>1 shapes failed, 8-15% mismatches, while
                     * every n_cbase==1 phase stayed clean). */
                    cbase_idx++;
                    ch_off += MAC_PD;
                }
            } else {
                in_writeout = false;
                cbase_idx = 0;
                ch_off = 0;
                w_ot_base += Cin;
                ot_out_ch_base += d.out_ch_stride;
                ot_idx++;
                wr_row = 0;
                wr_col = 0;
            }
        } else {
            k++;
            if (!in_writeout) {
                ch_off += MAC_PD;
            } else {
                if (wr_col == MAC_PC - 1) { wr_col = 0; wr_row++; }
                else { wr_col++; }
            }
        }
    }
}

/* A3 round (2026-08-24, ZHR-92, run_layer rewrite stage 4): DW's flat
 * pipeline, same technique as pw_flat_pipeline (stage 2/3), applied one
 * level shallower. DW's patch depends on `ot` itself (not just the
 * tile, unlike PW's disjoint Cin banking), so DW_PATCH_STAGE stays a
 * separate per-ot staging call in run_layer, UNCHANGED -- reverting it
 * to a compile-time bound was already tried and found ~5x more
 * expensive for K=3 layers (338 vs 72 real cycles, see its own header
 * comment), not reopened here. dw_one_ot_flat only replaces the
 * (f, tap, writeout) portion: what used to be DW_BT_STAGE + DW_WT_STAGE
 * + RESET + run_reduce_dw + WRITEOUT_DW as separate named regions per
 * (ot,f) becomes one counter-driven PIPELINE loop per ot, covering
 * every f internally (fpg is usually 1; 4 real layers need fpg=2).
 *
 * Compute-phase trip count is compile-time fixed at MAX_K*MAX_K=49 taps
 * regardless of real K (weight zero-filled for kh>=K||kw>=K, the same
 * established GATHER_ALL_DW zero-fill pattern -- this read pattern's
 * safety was already verified term-by-term for the un-flattened code
 * and carries over unchanged here) -- unlike DW_WT_STAGE's own
 * already-reverted MAX_K attempt, this is not optional: the flat
 * pipeline's counter needs a compile-time wrap bound to avoid a
 * runtime-derived loop bound (round 13's `kh = step / MAX_K` mistake).
 * Real board cost of this for K<7 layers (most of the network) is
 * unmeasured before this round -- flagging honestly, not assumed
 * cheap, since the analogous MAX_K choice already cost ~3x when tried
 * for DW_WT_STAGE specifically (98 vs 36 measured cycles at K=3).
 *
 * WRITEOUT keeps its boundary guard (c_sz/r_sz/col_sz) -- burst
 * inference stays explicitly out of scope this round (see
 * pw_flat_pipeline's own note): the same conditional-store shape
 * already failed to burst three times on the old WRITEOUT_DW. */
static void dw_one_ot_flat(
    const LayerDescV2 &d,
    const wt_t w_base[], const acc_t b_base[],
    const act_t dw_patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX],
    act_t out_base[],
    int rt, int colt, int ot, int c_sz, int r_sz, int col_sz)
{
    #pragma HLS ARRAY_PARTITION variable=dw_patch complete dim=0

    const int K = d.k, S = d.stride;
    const int n_f = d.fpg;
    const int total_iters = n_f * (MAX_K * MAX_K + MAC_PD * MAC_PR * MAC_PC);

    acc_t acc[MAC_PD][MAC_PR][MAC_PC];
    #pragma HLS ARRAY_PARTITION variable=acc complete dim=0

    int f_idx = 0;
    bool in_writeout = false;
    int kh = 0, kw = 0;
    int wr_dd = 0, wr_rr = 0, wr_cw = 0;

    /* per-f registers -- only MAC_PD (2) distinct values each, same
     * precompute-into-a-small-table technique as pw_flat_pipeline's
     * shift_reg / run_layer's existing oc_tbl/oc_ch_tbl/shift_tbl. */
    acc_t bias_reg[MAC_PD];
    int   wt_base_reg[MAC_PD];
    int   oc_ch_reg[MAC_PD];
    int   shift_reg[MAC_PD];
    #pragma HLS ARRAY_PARTITION variable=bias_reg complete dim=0
    #pragma HLS ARRAY_PARTITION variable=wt_base_reg complete dim=0
    #pragma HLS ARRAY_PARTITION variable=oc_ch_reg complete dim=0
    #pragma HLS ARRAY_PARTITION variable=shift_reg complete dim=0

    /* seed f=0's registers before the loop starts -- compute reads them
     * immediately at i==0, a cycle before the mid-loop reseed (below)
     * would first fire. */
    {
        int oc_val = ot * MAC_PD * d.fpg + 0;
        int ch_val = oc_val * d.out_ch_stride;
        int wtb    = oc_val * K * K;
        for (int dd0 = 0; dd0 < MAC_PD; dd0++) {
            #pragma HLS UNROLL
            bool v = (dd0 < c_sz);
            int oc_safe = v ? oc_val : 0;
            bias_reg[dd0]    = v ? b_base[d.b_off + oc_safe] : (acc_t)0;
            wt_base_reg[dd0] = wtb;
            oc_ch_reg[dd0]   = ch_val;
            shift_reg[dd0]   = d.use_shift_table ? (int)w_base[d.shift_off + oc_val] : d.out_shift;
            oc_val += d.fpg;
            ch_val += d.fpg * d.out_ch_stride;
            wtb    += d.fpg * K * K;
        }
    }

    DW_FLAT: for (int i = 0; i < total_iters; i++) {
        #pragma HLS PIPELINE II=1
        bool reset_acc = (!in_writeout) && (kh == 0) && (kw == 0);

        if (!in_writeout) {
            for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                bool valid = (kh < K) && (kw < K) && (dd < c_sz);
                wt_t wv = valid ? w_base[d.w_off + wt_base_reg[dd] + kh * K + kw] : (wt_t)0;
                for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        act_t iv = (S == 1)
                            ? dw_patch[dd][rr * 1 + kh][cw * 1 + kw]
                            : dw_patch[dd][rr * 2 + kh][cw * 2 + kw];
                        acc_t prod = (acc_t)iv * (acc_t)wv;
                        acc[dd][rr][cw] = reset_acc ? prod : (acc_t)(acc[dd][rr][cw] + prod);
                    }
                }
            }
        } else {
            if (wr_dd < c_sz && wr_rr < r_sz && wr_cw < col_sz) {
                int oh = rt * MAC_PR + wr_rr;
                int ow = colt * MAC_PC + wr_cw;
                out_base[d.out_off + oc_ch_reg[wr_dd] + oh * d.w_out + ow] =
                    (act_t)clip_shift(acc[wr_dd][wr_rr][wr_cw] + bias_reg[wr_dd], shift_reg[wr_dd]);
            }
        }

        /* counter update -- no division/modulo anywhere */
        if (!in_writeout) {
            if (kw == MAX_K - 1) {
                kw = 0;
                if (kh == MAX_K - 1) {
                    kh = 0;
                    in_writeout = true;
                } else {
                    kh++;
                }
            } else {
                kw++;
            }
        } else {
            if (wr_cw == MAC_PC - 1) {
                wr_cw = 0;
                if (wr_rr == MAC_PR - 1) {
                    wr_rr = 0;
                    if (wr_dd == MAC_PD - 1) {
                        wr_dd = 0;
                        in_writeout = false;
                        f_idx++;
                        /* reseed next f's registers -- harmless when
                         * f_idx==n_f (the loop's own last iteration);
                         * that result is never read. */
                        int oc_val = ot * MAC_PD * d.fpg + f_idx;
                        int ch_val = oc_val * d.out_ch_stride;
                        int wtb    = oc_val * K * K;
                        for (int dd0 = 0; dd0 < MAC_PD; dd0++) {
                            #pragma HLS UNROLL
                            bool v = (dd0 < c_sz);
                            int oc_safe = v ? oc_val : 0;
                            bias_reg[dd0]    = v ? b_base[d.b_off + oc_safe] : (acc_t)0;
                            wt_base_reg[dd0] = wtb;
                            oc_ch_reg[dd0]   = ch_val;
                            shift_reg[dd0]   = d.use_shift_table ? (int)w_base[d.shift_off + oc_val] : d.out_shift;
                            oc_val += d.fpg;
                            ch_val += d.fpg * d.out_ch_stride;
                            wtb    += d.fpg * K * K;
                        }
                    } else {
                        wr_dd++;
                    }
                } else {
                    wr_rr++;
                }
            } else {
                wr_cw++;
            }
        }
    }
}

/* ---- round 11: run_dwconv/run_pwconv are GONE. This is the only tile
 * driver in the design, and mac_array_top (below) calls it unconditionally
 * -- no more `if (op==DW) run_dwconv() else run_pwconv()`. That dispatch
 * was round 9/10's actual root cause (see run_reduce_unified's header):
 * as long as DW and PW were two separate functions, each got its own
 * private call site into whatever shared step function existed, no matter
 * what that function was named or how its own PIPELINE was structured.
 *
 * DW and PW's outer tiling genuinely differs in what varies per "output
 * tile" (`ot`): DW's `ot` is a channel-tile (8 channels, needs its own
 * patch+weight staging every ot); PW's `ot` is a single output channel
 * processed sequentially against spatial input staged ONCE per (rt,colt)
 * and reused across every ot. Rather than force identical staging cost
 * models onto both (not needed for this round's question -- DSP, not
 * staging efficiency), both keep their own STAGE/WSTAGE and WRITEOUT
 * bodies, branched on op_type. What's unconditional and genuinely shared:
 * the (rt,colt,ot) loop nest itself (one nest, reused by both), RESET,
 * and the single call into run_reduce_unified. */
static void run_layer(const LayerDescV2 &d,
                       const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                       act_t out_base[], const ap_uint<32> in_base_wide[])
{
    const int Hin = d.h_in, Win = d.w_in;
    const int K = d.k, S = d.stride, P = d.pad;
    const int patch_r = (MAC_PR - 1) * S + K;
    const int patch_c = (MAC_PC - 1) * S + K;
    const int Cin = d.cin, H = d.h_in, W = d.w_in;

    const int n_ot = (d.op_type == LDESC_OP_DWCONV) ? d.n_ch_tiles : d.cout;

    /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2): the old
     * `n_cbase` local (PW's Cin-chunking count) is GONE -- pw_flat_pipeline
     * computes its own copy internally now, chunking is entirely PW's own
     * concern, not shared with run_layer's DW-only code below. */

    /* A3 round (2026-08-21, ZHR-92): weight hoist -- ATTEMPTED AND
     * REVERTED (2026-08-22, same round as the DW loop-bound fix above).
     * pw_weight_cache (442,368 elements = ~432KB) was the #2 worst P&R
     * timing path (PW_WEIGHT_HOIST -> pw_weight_cache BRAM write, 95%
     * route-delay-dominated) and a major share of BRAM's 89-95%
     * congestion, for a measured gain of only 1.3% (700.27ms -> 690.37ms)
     * -- a bad trade once the design is timing-negative. Reverted to
     * direct per-(ot,cbase) DRAM reads in PW_WSTAGE below (the pre-hoist
     * form, restored verbatim from that read site's own preserved
     * comment). pw_bias_cache is untouched: at 1152 elements (~4.6KB)
     * it's not implicated in either worst path and wasn't part of this
     * round's diagnosis -- reverting it would be an unrequested change. */
    static acc_t pw_bias_cache[MAX_PW_BIAS_CACHE];
    if (d.op_type == LDESC_OP_PWCONV) {
        PW_BIAS_HOIST: for (int oc = 0; oc < d.cout; oc++) {
            #pragma HLS PIPELINE II=1
            pw_bias_cache[oc] = b_base[d.b_off + oc];
        }
    }

    for (int rt = 0; rt < d.n_row_tiles; rt++) {
        int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
        for (int colt = 0; colt < d.n_col_tiles; colt++) {
            int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

            /* A3 round 2 (2026-08-21, ZHR-92): loop-invariant hoist, same
             * pattern as the weight+bias hoist above -- PW's spatial patch
             * depends only on (rt,colt), not ot, but PW_STAGE used to
             * restage it from DRAM once per (rt,colt,ot,cbase). Stage the
             * full-Cin patch for THIS (rt,colt) tile once, here, reused by
             * every ot/cbase below. No boundary check needed (PW is always
             * k=1/stride=1/pad=0, no receptive-field overhang past r_sz/
             * col_sz -- same as the original PW_STAGE). Dummy/unused for
             * DW (guarded by the op_type check, same convention as the
             * weight hoist). */
            static act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC];
            /* A3 round (2026-08-23, ZHR-92): full partition, matching what
             * the chunk-local pw_patch used to have at 1/36th the depth --
             * same cyclic-by-MAC_PD / complete-dims-2,3 shape, just applied
             * to the real MAX_CIN=1152-deep storage instead of a 32-deep
             * copy of it. This is what actually eliminates PW_STAGE's copy
             * below: GATHER_ALL_PW (in run_reduce_unified) now reads this
             * array directly at a per-lane-parallel-addressable offset,
             * the same property the copy used to exist purely to provide. */
            #pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic factor=MAC_PD dim=1
            #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
            #pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3
            /* A3 round 5 (2026-08-22, ZHR-92) -- ATTEMPTED AND REVERTED,
             * kept here as a TODO, not a dead end. PW_STAGE (the copy from
             * pw_patch_full into the smaller per-cbase pw_patch, below) is
             * a real, correctly-diagnosed cost: ~71% of the whole 179ms
             * (127ms), confirmed via the csynth report's own module
             * latency breakdown, not inferred. The fix (have
             * run_reduce_unified read pw_patch_full directly, fully
             * parallel-partitioned, eliminating PW_STAGE) is also
             * correct -- csim 15/15, GATHER_ALL_PW held II=1. What killed
             * it was resource headroom, not the approach: a pragma-only
             * probe undersold the real cost (+2.7% LUT/+16 BRAM_18K
             * estimated vs +32 BRAM_18K/real-BRAM-95%/WNS -1.289ns-even-
             * after-phys_opt_design actually measured) -- the probe never
             * went through real P&R, and at 90%+ utilization this
             * project's own "real P&R beats the csynth estimate by ~25
             * points" experience stopped holding. Reverted to this
             * (PW_STAGE-intact, 179.38ms, board-verified byte-exact)
             * state as the known-good baseline for the end-to-end round.
             * Three ready-to-try restart paths once resource margin exists
             * again, none attempted yet: (1) drop array width 32->16 to
             * free enough BRAM/LUT for the full parallel partition; (2) a
             * descriptor flag letting the generator choose PW_STAGE vs.
             * direct-read per layer, keeping the fix for small/medium
             * layers where it already fits and falling back for large
             * ones; (3) pair this with option 2's ot-tiled accumulator
             * (also costed, not implemented) to cap worst-case storage
             * instead of needing pw_patch_full's full width live at once.
             *
             * RETRIED this round (2026-08-23, ZHR-92): resource conditions
             * are now BRAM 9.3%/LUT 58.85%/WNS +0.115ns (vs. 95%/95%/
             * -1.289ns above) after the pw_weight_cache revert and the
             * gmem_act_wide merge freed real headroom -- the +32 BRAM_18K
             * this attempt actually cost last time is ~32% of today's
             * budget, not ~100%+. None of the three restart paths above
             * were needed; retrying the original direct approach as-is.
             * Per the lesson already recorded two paragraphs up, the
             * csynth numbers below are a magnitude check only -- the
             * actual go/no-go judgment is real P&R (see round writeup on
             * ZHR-92 for the P&R result this comment predates). */
            if (d.op_type == LDESC_OP_PWCONV) {
                /* A3 round (2026-08-22, ZHR-92, code-review followup):
                 * rr/cw used to be bounded by the runtime r_sz/col_sz --
                 * same class of hazard as the DW fix above (a loop trip
                 * count, not just a data value, depending on a runtime
                 * comparison), just never the one that happened to blow up:
                 * pw_patch_full's dims 2/3 aren't ARRAY_PARTITIONed (plain
                 * BRAM, not per-element registers), so this specific site
                 * was never actually at risk -- but bounding it at the
                 * compile-time MAC_PR/MAC_PC with a data-path `valid` closes
                 * the pattern structurally instead of relying on that being
                 * true forever. Address clamped to a known-in-range
                 * constant (0) when invalid, matching the DW fix's
                 * discipline. */
                /* A3 round (2026-08-23, ZHR-92, MERGE): the wide/narrow
                 * branch split (see git history for the pre-merge form) is
                 * GONE -- both cases now go through ONE word read via
                 * in_base_wide, unconditionally, regardless of
                 * d.use_wide_path (that field is no longer read here; see
                 * its header comment). Motivation, from the P&R round that
                 * added the 5th master (solution18, WNS -0.292ns): the
                 * critical path was NOT gmem_meta-related (Option E's old
                 * "zero logic levels, 74% routing delay" signature) but
                 * entirely internal to run_layer -- an FSM state bit
                 * driving a 32x32 multiplier's DSP cascade register (4
                 * logic levels, 58% logic delay). Prime suspect: the wide
                 * and narrow branches each computed their own
                 * `ci * d.in_ch_stride` product, and being mutually
                 * exclusive (only one runs per call), HLS's resource
                 * binding pass plausibly shared ONE physical multiplier
                 * between them, gated by an FSM-state mux feeding its
                 * operand register -- matching the observed path exactly.
                 * Supporting (not conclusive) evidence: DSP only rose
                 * 63->73 (+10) adding the wide path, not the ~35-70 a
                 * second independent 32x32 multiply chain would cost if
                 * unshared. This merge removes the second branch's
                 * multiply entirely (one `ci * d.in_ch_stride` site, not
                 * two) AND removes the separate gmem_act_wide master
                 * (folded back onto bundle=gmem_act, below) -- both changes
                 * follow from the same root decision (one unified read
                 * path), tested together as this round's one variable; the
                 * causal claim that this fixes the FSM->DSP path is NOT
                 * verified yet, only P&R can confirm it (see round report).
                 * Byte addressing is a general mod-4 decomposition of the
                 * SAME address the old narrow path used (d.in_off +
                 * ci*d.in_ch_stride + oh*W + colt*MAC_PC + cw), not an
                 * alignment assumption -- see the followup comment below
                 * for why a naive single-word-read version of this wasn't
                 * actually correct in general, and the fix. Csim coverage:
                 * Phase6 (desc6[1]/desc6[3], cin=10/4, h_in=w_in=1,
                 * in_off=360/374 -- 374 is NOT 4-aligned) covers the
                 * layer-50/51 narrow shape; Phase4 (desc4[1], cin=32,
                 * w_out=10, genuine partial last column tile col_sz=2)
                 * covers the wide-with-partial-tile case. No new phase
                 * needed -- both were pre-existing tests. */
                /* A3 round (2026-08-23, ZHR-92, MERGE followup): FOUND AND
                 * FIXED via csim, not assumed correct -- the first version
                 * of this loop (one word read per (ci,rr), lane=base_lane+cw)
                 * crashed csim ("Hi(39) out of bound(32) in range()") on
                 * Phase4's desc4[1] (PW, cin=32, w_out=10 -> a genuine
                 * PARTIAL last column tile, col_sz=2, at a non-4-aligned
                 * offset -- a pre-existing round-9 synthetic shape, never
                 * designed with word-alignment in mind). The "wide layers
                 * are always 4-aligned" invariant only holds for the real
                 * 82-entry network (verified there), not for arbitrary
                 * csim shapes -- and csim has to pass on ALL of them, not
                 * just the production-relevant ones. Fix: general 2-word
                 * read, second word fetched ONLY when a valid cw's byte
                 * would actually fall in it (need_word1) -- for the
                 * aligned/full-tile case (every real wide-eligible layer)
                 * need_word1 is always false, so this costs zero extra
                 * reads versus the original single-word design; the extra
                 * read only happens for misaligned/partial tiles like
                 * Phase4's, which is correctness insurance, not something
                 * the real network pays for. */
                /* A3 round (2026-08-23, ZHR-92, accumulator rewrite): three
                 * independent P&R rounds (wide-port, merge, PW_STAGE
                 * elimination) all found the SAME critical path -- an FSM
                 * state bit driving mul_32s_32s_32_2_1's DSP cascade
                 * register, logic levels climbing 4->4->5 each round even
                 * though only the LAST round touched DW's own code. Root
                 * cause: every `index * stride`-shaped address term in
                 * run_layer (this one, DW_PATCH_STAGE's, DW_WT_STAGE's,
                 * WRITEOUT_DW's, WRITEOUT_PW's) is getting bound to ONE
                 * shared physical multiplier, selected by FSM state --
                 * each additional call site deepens that selection tree by
                 * one level. Fix, applied at all five sites together (same
                 * mechanism, same round, same attribution as the
                 * DW_WT_STAGE+DW_PATCH_STAGE precedent): replace
                 * `ci * d.in_ch_stride` with a loop-carried accumulator
                 * (in_ch_base, incremented by d.in_ch_stride once per ci
                 * step) -- an ADD, not a multiply, so there is no multiply
                 * left here to bind. NOT the same shape as round 2's
                 * rejected accumulator attempt (that one was INSIDE the
                 * 512-wide UNIFIED unrolled region, needing one accumulator
                 * PER LANE, +5 DSP/+247 LUT for the privilege); this is an
                 * ordinary sequential staging loop, one accumulator total. */
                int in_ch_base = 0;   // == ci * d.in_ch_stride, incremented below
                PW_PATCH_HOIST: for (int ci = 0; ci < Cin; ci++) {
                    for (int rr = 0; rr < MAC_PR; rr++) {
                        bool r_valid = rr < r_sz;
                        int oh = rt * MAC_PR + (r_valid ? rr : 0);
                        int base_idx = d.in_off + in_ch_base + oh * W + colt * MAC_PC;
                        int word_addr0 = base_idx >> 2;
                        bool need_word1 = r_valid && (((base_idx & 3) + col_sz) > 4);
                        ap_uint<32> packed0 = r_valid   ? in_base_wide[word_addr0]     : (ap_uint<32>)0;
                        ap_uint<32> packed1 = need_word1 ? in_base_wide[word_addr0 + 1] : (ap_uint<32>)0;
                        for (int cw = 0; cw < MAC_PC; cw++) {
                            bool valid = r_valid && (cw < col_sz);
                            int elem_idx = base_idx + cw;
                            int lane = elem_idx & 3;
                            bool second = (elem_idx >> 2) != word_addr0;
                            pw_patch_full[ci][rr][cw] = valid
                                ? (act_t)(second ? packed1 : packed0).range(lane * 8 + 7, lane * 8)
                                : (act_t)0;
                        }
                    }
                    in_ch_base += d.in_ch_stride;
                }
            }

            /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2):
             * PW no longer goes through this per-ot loop at all -- see
             * pw_flat_pipeline's own header comment above run_layer.
             * The old shared `ot_out_ch_base` accumulator (PW-only, per
             * its own removed comment) went with it; DW's own per-channel
             * addressing (oc_ch_tbl, below) never used it. */
            if (d.op_type == LDESC_OP_DWCONV) {
            for (int ot = 0; ot < n_ot; ot++) {
                /* known simplification, not yet addressed: DW's receptive-
                 * field reads overlap between spatial lanes (sliding
                 * window), so unlike PW's disjoint Cin banking there is no
                 * clean compile-time-constant bank assignment here without
                 * a real line-buffer/shift-register redesign -- kept
                 * `complete` (register-file, correct but not necessarily
                 * cheap). */
                act_t dw_patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX];
                #pragma HLS ARRAY_PARTITION variable=dw_patch complete dim=0

                int c_sz = MAC_PD;

                /* A2 pre-step (2026-08-21): fpg (filters-per-group) was
                 * carried in LayerDescV2 but never read anywhere in this
                 * file -- DW implicitly assumed fpg=1 (cout==cin, one
                 * filter per input channel). Real network has 4 layers
                 * (3 stage-downsamples + final_conv) at fpg=2: each INPUT
                 * channel produces fpg INDEPENDENT output channels, each
                 * with its own K*K filter, but still only reducing over
                 * that SAME single input channel's receptive field (no
                 * cross-channel accumulation -- still depthwise-shaped,
                 * just multiple filters per input channel instead of one).
                 * Same failure class as the K=7 gap: silently wrong on
                 * those 4 layers today, no crash. Fix: patch staging
                 * (reads the input, doesn't depend on which output filter)
                 * stays a single per-ot step; weight/bias staging, RESET,
                 * the MAC reduction, and WRITEOUT now repeat for f=0..fpg-1
                 * -- since stage 4 (2026-08-24), that repetition lives
                 * entirely inside dw_one_ot_flat (it reads d.fpg directly),
                 * not as a `for(f...)` loop here. */
                if (d.op_type == LDESC_OP_DWCONV) {
                    c_sz = (ot == d.n_ch_tiles - 1) ? d.last_ch_tile : MAC_PD;
                    /* A3 round (2026-08-22, ZHR-92): c_sz is a runtime value
                     * (derived from d.n_ch_tiles/d.last_ch_tile, both read
                     * off the gmem_meta AXI path -- P&R's own critical path,
                     * WNS=+0.112ns, zero logic levels/74% routing delay/
                     * fanout=70). This loop's bound used to BE c_sz directly
                     * (`for (cc < c_sz)`), making the loop's own trip count
                     * -- not just a data value -- depend on that razor-thin
                     * timing path. csim (which just executes C in program
                     * order) can never see this; only real silicon can. This
                     * is board-confirmed as the root cause of DW's 46%
                     * mismatch (all of it landing on cc=1/dd=1, the second
                     * MAC_PD lane, which is exactly the lane whose write
                     * depends on the loop actually reaching a 2nd iteration).
                     * Fix, same technique as round 8's PW zero-fill: bound
                     * fixed at the compile-time constant MAC_PD, validity
                     * pushed into a plain data-path bool instead of loop
                     * control. Address clamped to a known-in-range constant
                     * (0, not min(oc,cout-1)) when invalid, so no runtime
                     * comparison is needed to keep the read in-bounds --
                     * the read result itself is discarded either way. */
                    /* A3 round (2026-08-22, ZHR-92, followup): pr/pc bound
                     * reverted from the compile-time PATCH_R_MAX/
                     * PATCH_C_MAX back to the runtime patch_r/patch_c --
                     * same recoverable-cost pattern as DW_WT_STAGE's
                     * MAX_K->K revert just below/above: at PATCH_R_MAX x
                     * PATCH_C_MAX=13x13=169/channel this loop iterates the
                     * worst-case (K=7,S=2) grid regardless of this layer's
                     * real K/S, measured 352 cycles for entry5 (K=3,S=1,
                     * real 6x6=36/channel) -- 338 of 338 total iterations
                     * run, only 72 needed.
                     * Protection mechanism verified term-by-term, not
                     * assumed (this is what makes reverting safe, same as
                     * DW_WT_STAGE's dd case): GATHER_ALL_DW's
                     * valid=(kh<dw_K)&&(kw<dw_K) mask sits on the WEIGHT
                     * side only --
                     *   lane_w_all[step][dd]  = valid ? dw_wtile[...] : 0;
                     *   lane_in_all[step][dd][rr][cw] = dw_patch[dd][rr*S+kh][cw*S+kw];  // no valid check here
                     * -- so for any kh>=dw_K (equivalently pr=rr*S+kh
                     * beyond this layer's real patch_r), the corresponding
                     * weight is forced to 0 and acc += lane_in*0 stays 0
                     * regardless of what dw_patch holds there. The read
                     * index's own upper bound is (MAC_PR-1)*S+(MAX_K-1),
                     * i.e. up to PATCH_R_MAX-1 -- for K=3/S=1 that's index
                     * 9, while this loop now only WRITES up to index
                     * patch_r-1=5; indices 6..9 stay uninitialized
                     * registers, but every step that reads them has
                     * kh>=K=3, so its weight is 0 and the read value never
                     * reaches acc. Holds BECAUSE dw_patch's declared size
                     * stays PATCH_R_MAX x PATCH_C_MAX (only the loop bound
                     * changed, not the array) -- shrinking the declaration
                     * itself would make those same reads genuinely
                     * out-of-bounds instead of merely unwritten. DO NOT
                     * shrink dw_patch's declared dimensions. */
                    /* A3 round (2026-08-23, ZHR-92, accumulator rewrite):
                     * see PW_PATCH_HOIST's header comment for the full
                     * rationale (same round, same mechanism, five sites).
                     * ch_off replaces `c * d.in_ch_stride` -- seeded once
                     * per (rt,colt,ot) at cc=0's value (one multiply,
                     * versus the ORIGINAL up to patch_r*patch_c=169
                     * re-evaluations of the same multiply per cc, since it
                     * used to sit inside the pr/pc nest), then advanced by
                     * a plain add per cc step. When cc is invalid (c_sz=1,
                     * only possible at cc=MAC_PD-1), ch_off's post-add
                     * value no longer matches the old code's clamp-to-0
                     * behavior -- harmless, because the array read that
                     * would use it is gated by `valid` in the SAME way the
                     * old code gated it (v stays 0, in_base is never
                     * actually read), same reasoning already established
                     * for DW_WT_STAGE's kh/kw revert elsewhere in this
                     * file: an invalid index feeding a value that's
                     * discarded, not an out-of-bounds read. `c` itself is
                     * no longer needed (was only ever used in this one
                     * multiply). */
                    int ch_off = ot * MAC_PD * d.in_ch_stride;
                    DW_PATCH_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                        bool valid = (cc < c_sz);
                        for (int pr = 0; pr < patch_r; pr++) {
                            int ih = rt * MAC_PR * S - P + pr;
                            for (int pc = 0; pc < patch_c; pc++) {
                                int iw = colt * MAC_PC * S - P + pc;
                                act_t v = 0;
                                if (valid &&
                                    ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                    v = in_base[d.in_off + ch_off + ih * Win + iw];
                                dw_patch[cc][pr][pc] = v;
                            }
                        }
                        ch_off += d.in_ch_stride;
                    }
                }

                /* A3 round (2026-08-24, ZHR-92, run_layer rewrite stage 4):
                 * DW_BT_STAGE + DW_WT_STAGE + RESET + run_reduce_dw +
                 * oc_tbl/oc_ch_tbl/shift_tbl + WRITEOUT_DW (previously a
                 * `for(f...) {...}` loop with all of the above repeated
                 * per f) replaced by one call -- see dw_one_ot_flat's own
                 * header comment above run_layer for the full rationale.
                 * dw_wtile/dw_btile (declared above, alongside dw_patch)
                 * are no longer used -- the flat pipeline reads weight/
                 * bias directly, no separate staging arrays. */
                dw_one_ot_flat(d, w_base, b_base, dw_patch, out_base, rt, colt, ot, c_sz, r_sz, col_sz);
            }
            } else {
                /* A3 round (2026-08-23, ZHR-92, run_layer rewrite stage 2):
                 * PW's whole (ot,cbase) reduce + WRITEOUT_PW, replaced by
                 * one call -- see pw_flat_pipeline's own header comment
                 * above run_layer for the full rationale. Called once per
                 * (rt,colt) tile (unlike DW's per-ot loop above), since the
                 * flat pipeline handles every ot/cbase internally. */
                pw_flat_pipeline(d, w_base, pw_patch_full, pw_bias_cache, out_base, rt, colt, r_sz, col_sz);
            }
        }
    }
}

/* ---- Phase A1 (2026-08-20): elementwise residual Add, two DRAM sources
 * (op0=in_off, op1=in2_off, confirmed from tools/layer_dag_ground_truth.json's
 * multi_input_nodes -- every real Add in the network reads the
 * token_mixer/identity branch and the layer_scale/processed branch),
 * one write. No MAC array involvement, so this being a separate function
 * with its own call site (see mac_array_top below) does NOT reintroduce
 * round 9/10's duplication problem -- that was specifically about two
 * callers needing the SAME expensive shared 64-wide MAC array; Add shares
 * no resource with run_layer's DW/PW path, there's nothing to duplicate.
 * Self-verified writeback (ZHR-91 row 6): exercised via
 * mac_array_tb.cpp's existing fault-injection harness pattern, extended
 * to target an Add layer specifically -- the historical defect-5 (Add
 * write-back failure on the old architecture, root cause never isolated)
 * is untested on this architecture until that check runs and passes. */
static void run_add(const LayerDescV2 &d,
                     const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    ADD: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        acc_t sum = (acc_t)in_base[d.in_off + i] + (acc_t)in_base[d.in2_off + i];
        out_base[d.out_off + i] = (act_t)clip_shift(sum, d.out_shift);
    }
}

/* ---- Phase A1 (SE block, priority 1 per user direction 2026-08-20):
 * GAP is the only operator in the whole design with a genuine full-tensor
 * read-before-write dependency -- every other op (DW/PW/Add/ReLU/Sigmoid/
 * Scale) can stream one output per some small constant number of reads.
 * GAP has to consume all H*W pixels of a channel before it can produce
 * that channel's single output value. This is the "does it expose
 * buffer/descriptor design gaps" case the whole SE-first ordering was
 * chosen for (see the interface sketch, ZHR-92: GAP flagged there as the
 * one op needing different layer-controller scheduling treatment, not a
 * new port). Division by H*W (not a power of 2 for arbitrary real layer
 * shapes) is a real integer divide here -- functionally correct for A1's
 * csim-level goal, but a synthesis-cost item to revisit later, not
 * solved now (matches the project's "don't optimize efficiency this
 * round" discipline). */
static void run_gap(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int HW = d.h_in * d.w_in;
    GAP_C: for (int c = 0; c < d.cin; c++) {
        acc_t sum = 0;
        GAP_HW: for (int i = 0; i < HW; i++) {
            #pragma HLS PIPELINE II=1
            sum += (acc_t)in_base[d.in_off + c * HW + i];
        }
        acc_t avg = sum / HW;
        out_base[d.out_off + c] = (act_t)clip_shift(avg, d.out_shift);
    }
}

static void run_relu(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    RELU: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        act_t v = in_base[d.in_off + i];
        out_base[d.out_off + i] = (v < 0) ? act_t(0) : v;
    }
}

/* PLACEHOLDER quantized sigmoid -- a crude monotonic saturating linear
 * map, NOT calibrated against any real sigmoid curve. Proves the
 * GAP->fc1->ReLU->fc2->Sigmoid->Scale data flow wires together and
 * produces a plausible gate; numeric accuracy is explicitly out of scope
 * for A1 (that's calibration/quantization work, later). Flagging this
 * now rather than letting it silently become a landmine -- this project
 * has already been burned once by an unflagged placeholder
 * (calibrate_activations.py's default_act_scale, 37x off, undiscovered
 * for 9+ rounds). Reused (not shared code, but the same intent) by
 * run_gelu below -- GELU(x)~=x*sigmoid(1.702x) is a standard documented
 * approximation, so using this same placeholder gate for both keeps the
 * two "uncalibrated" surfaces consistent instead of inventing a second,
 * unrelated placeholder shape for GELU. */
static act_t quantized_sigmoid(act_t x)
{
    acc_t v = (acc_t)x + 64;
    if (v > 127) v = 127;
    if (v < 0)   v = 0;
    return (act_t)v;
}

static void run_sigmoid(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    SIGMOID: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        out_base[d.out_off + i] = quantized_sigmoid(in_base[d.in_off + i]);
    }
}

/* GELU, single source (op0 only) -- real ONNX pattern is Div->Erf->Add->
 * Mul (17 instances, confirmed via direct node inspection, see ZHR-64);
 * this is the ATOMIC hardware op a folded descriptor entry dispatches to,
 * not a re-implementation of the 4-node decomposition. gelu(x)~=x*sigmoid
 * (1.702x), computed here as x*quantized_sigmoid(x) -- same placeholder-
 * accuracy caveat as run_sigmoid, proves data flow not numeric fidelity.
 * tools/gen_gelu_lut.py exists as the real calibrated asset to integrate
 * when this needs actual accuracy (not this round). */
static void run_gelu(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int total = d.cin * d.h_in * d.w_in;
    GELU: for (int i = 0; i < total; i++) {
        #pragma HLS PIPELINE II=1
        act_t x = in_base[d.in_off + i];
        acc_t prod = (acc_t)x * (acc_t)quantized_sigmoid(x);
        out_base[d.out_off + i] = (act_t)clip_shift(prod, d.out_shift);
    }
}

/* SE's final gate multiply: op0 (in_off) is the full HxWxC feature map,
 * op1 (in2_off) is the C-length gate, broadcast over spatial -- confirmed
 * from tools/layer_dag_ground_truth.json: final_conv's node has fan_out=2,
 * feeding both ReduceMean AND this Mul directly from the same tensor. */
static void run_scale(const LayerDescV2 &d, const act_t in_base[], act_t out_base[])
{
    const int HW = d.h_in * d.w_in;
    SCALE_C: for (int c = 0; c < d.cin; c++) {
        act_t gate = in_base[d.in2_off + c];
        SCALE_HW: for (int i = 0; i < HW; i++) {
            #pragma HLS PIPELINE II=1
            acc_t prod = (acc_t)in_base[d.in_off + c * HW + i] * (acc_t)gate;
            out_base[d.out_off + c * HW + i] = (act_t)clip_shift(prod, d.out_shift);
        }
    }
}

/* A3 interface resource-budget check (2026-08-21, ZHR-92). Round 5-15's
 * resource numbers are compute-core-only (HW Interfaces section confirmed
 * every port ap_none/ap_vld, no #pragma HLS INTERFACE anywhere) -- this
 * is the first csynth run with a real m_axi interface, specifically to
 * get a real number before committing to any further A3 implementation.
 * 4 masters, matching the old architecture's proven 17->4 consolidation
 * (ZHR-8) rather than one bundle per array: gmem_act shared by in_base/
 * out_base (already the SAME physical buffer under Route C -- the ARM
 * writes Stem's output and reads the final result from the same region),
 * gmem_w for weights (includes the appended per-channel shift table),
 * gmem_b for bias, gmem_meta for desc+out_written (small, low-bandwidth
 * control/descriptor traffic, doesn't need its own high-bandwidth path).
 * desc stays ap_none-shaped in this signature deliberately -- the
 * reviewed interface sketch's DRAM-resident, read-one-entry-at-a-time
 * `desc` table is real A3 work, not needed just to get a first resource
 * number; m_axi on the same struct-array parameter is enough to see
 * whether AXI infrastructure fits the budget at all before doing that. */
void mac_array_top(
    const LayerDescV2 desc[],
    int n_layers,
    const act_t  in_base[],
    const wt_t   w_base[],
    const acc_t  b_base[],
    act_t        out_base[],
    int          out_written[],
    const ap_uint<32> in_base_wide[])
{
#pragma HLS INTERFACE m_axi port=desc         offset=slave bundle=gmem_meta
#pragma HLS INTERFACE m_axi port=in_base      offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=w_base       offset=slave bundle=gmem_w
#pragma HLS INTERFACE m_axi port=b_base       offset=slave bundle=gmem_b
#pragma HLS INTERFACE m_axi port=out_base     offset=slave bundle=gmem_act
#pragma HLS INTERFACE m_axi port=out_written  offset=slave bundle=gmem_meta
/* A3 round (2026-08-23, ZHR-92, MERGE): back on bundle=gmem_act, sharing
 * the SAME physical master as in_base/out_base -- the standalone
 * gmem_act_wide master (previous round, solution18) is gone. That
 * earlier round's justification for a SEPARATE bundle no longer applies:
 * the "limited memory ports" II violation it was avoiding happened when
 * TWO DIFFERENT bundles were both accessed inside ONE pipelined loop
 * region (wide/narrow branch INSIDE the loop, both statically scheduled
 * even though only one runs). That branch is gone now too (see
 * PW_PATCH_HOIST above) -- in_base_wide is PW_PATCH_HOIST's ONLY
 * accessor, in its own loop region, so this is architecturally the same
 * "one bundle, multiple ports, never contended within one region" shape
 * run_add already uses today (in_base x2 + out_base, all bundle=gmem_act,
 * inside ONE pipelined ADD loop, uncontested) -- not a new pattern, reusing
 * a load-bearing one already proven in this file. Motivation: the
 * standalone master's own HLS-side adapter cost 657 LUT/671 FF/2 BRAM18K
 * by itself (~30% of that round's total LUT growth), before counting the
 * BD-level SmartConnect's added crossbar port -- removing it is expected
 * to both shrink LUT and return the AXI master count to 4. Whether it
 * also resolves the FSM->DSP critical path (see PW_PATCH_HOIST's comment)
 * is the thing THIS round's P&R actually tests, not assumed here. */
#pragma HLS INTERFACE m_axi port=in_base_wide offset=slave bundle=gmem_act
#pragma HLS INTERFACE s_axilite port=n_layers bundle=control
#pragma HLS INTERFACE s_axilite port=desc bundle=control
#pragma HLS INTERFACE s_axilite port=in_base bundle=control
#pragma HLS INTERFACE s_axilite port=w_base bundle=control
#pragma HLS INTERFACE s_axilite port=b_base bundle=control
#pragma HLS INTERFACE s_axilite port=out_base bundle=control
#pragma HLS INTERFACE s_axilite port=out_written bundle=control
#pragma HLS INTERFACE s_axilite port=in_base_wide bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control
    /* A3 round (2026-08-22, ZHR-92): option D (read desc[i] into a local
     * on-chip copy once per layer, on the theory that gmem_meta's critical
     * path was caused by scattered per-field AXI reads/high fan-out) was
     * ATTEMPTED AND REVERTED -- placement failed outright (real LUT 66,317
     * vs 53,200 available, 25% over capacity; the whole-struct assignment
     * synthesized real burst-FIFO/decode logic, visible even at csynth as
     * a BRAM_18K jump from 31 to 141). More importantly the diagnosis
     * behind it was wrong: the first P&R run's own record already said
     * "zero logic levels, 74% routing delay, fanout=70" -- fanout=70 is
     * not high for a 704-bit bus reaching all of run_layer (expected
     * hundreds+), and "zero logic levels + 74% routing" describes a
     * PLACEMENT DISTANCE problem (gmem_meta's AXI interface logic placed
     * physically far from run_layer's registers), not a fan-out problem.
     * That record was available and cited before option D was designed,
     * just not read carefully enough. See option E (pblock) for the actual
     * distance-targeted fix. */
    for (int i = 0; i < n_layers; i++) {
        switch (desc[i].op_type) {
            case LDESC_OP_ADD:     run_add(desc[i], in_base, out_base); break;
            case LDESC_OP_GAP:     run_gap(desc[i], in_base, out_base); break;
            case LDESC_OP_RELU:    run_relu(desc[i], in_base, out_base); break;
            case LDESC_OP_SIGMOID: run_sigmoid(desc[i], in_base, out_base); break;
            case LDESC_OP_SCALE:   run_scale(desc[i], in_base, out_base); break;
            case LDESC_OP_GELU:    run_gelu(desc[i], in_base, out_base); break;
            default:                run_layer(desc[i], in_base, w_base, b_base, out_base, in_base_wide); break;
        }
        out_written[i] = 1;
    }
}
