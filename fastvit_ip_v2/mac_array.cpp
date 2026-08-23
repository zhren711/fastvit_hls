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

/* ---- A3 round 3 (2026-08-21, ZHR-92): drive_mac is GONE, its body
 * folded directly into UNIFIED's loop. Round 14's drive_mac existed as a
 * SEPARATE function specifically so HLS's resource sharing across this
 * function's two mutually-exclusive (op_type-gated) call sites kept it
 * to one physical 512-wide instance -- confirmed working ever since (DSP
 * stayed ~110-118 across every round, never doubled to ~1024). But that
 * same function-call boundary is exactly what prevented DW_TAP_H/W and
 * UNIFIED_PW from ever carrying their own #pragma HLS PIPELINE --
 * putting PIPELINE on either CALLER loop was round 9/10's original
 * mistake (duplicated the shared instance, DSP 512->1024), since a
 * pipelined caller loop demands its own dedicated per-cycle-available
 * copy of whatever it calls. Measured cost of leaving it this way (A3
 * round 2, ZHR-92, board-measured): ~61 cycles per drive_mac invocation
 * for one pipeline step's worth of real 512-wide work -- call/return
 * handshake (`INLINE off`), lane_in/lane_w parameter marshalling, and
 * acc round-tripping through the function interface, paid EVERY step.
 *
 * Fix: checked first (not assumed) that drive_mac really did have two
 * textual call sites (grep confirmed: one in the DW branch, one in the
 * PW branch, both below) -- inlining drive_mac directly would remove the
 * function boundary the shared-instance property depends on, almost
 * certainly reintroducing round 9/10's DSP doubling. Instead: gather
 * EVERY step's lane_in/lane_w BEFORE the reduction, into
 * lane_in_all/lane_w_all (indexed by step -- cheap on-chip copying, not
 * unrolled, not the throughput-critical path), then run ONE single
 * pipelined loop (UNIFIED) that reads a step at a time and does the
 * 512-wide accumulate directly, inline, with no function call inside the
 * pipelined region at all. There is exactly ONE 512-wide unrolled
 * accumulate region in the whole design -- same "only one physical
 * instance" property round 14 achieved, just enforced by there being
 * only one copy of the code, not by a function boundary.
 *
 * Two pitfalls this deliberately avoids (both already paid for once, in
 * round 12/13's history):
 *   - op_type branching INSIDE the pipelined region: not done here --
 *     UNIFIED only ever indexes by `step`, a plain induction variable;
 *     op_type only selects which GATHER code fills lane_in_all/
 *     lane_w_all beforehand, outside the pipelined loop entirely.
 *   - runtime-derived indices (kh/kw from a flat step counter, dw_S-
 *     dependent strides) INSIDE the pipelined region: round 12's actual
 *     failure was `dw_patch[dd][rr*dw_S+kh][cw*dw_S+kw]` evaluated
 *     per-lane inside the 512-way unroll, which fanned into ~3000+
 *     sparsemux cores just to hold II=1. That same stride-dependent
 *     gather still exists here, but ONLY in the GATHER phase (ordinary
 *     loop, not unrolled, not throughput-critical) -- the exact same
 *     dw_S==1/dw_S==2 branch technique GATHER_DW_D_S1/S2 already used is
 *     reused verbatim so the multiply stays compile-time-resolvable.
 *     UNIFIED itself reads lane_in_all[step][dd][rr][cw] -- a clean,
 *     compile-time-shaped index into an already-gathered buffer, with
 *     nothing runtime-derived left to fan out. */
static void run_reduce_unified(
    int op_type,
    int n_steps,
    /* DW operands -- read only when op_type==LDESC_OP_DWCONV */
    const act_t dw_patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX],
    const wt_t  dw_wtile[MAC_PD][MAX_K][MAX_K],
    int dw_K, int dw_S,
    /* PW operands -- read only when op_type==LDESC_OP_PWCONV */
    const act_t pw_patch[MAX_CIN_PW][MAC_PR][MAC_PC],
    const wt_t  pw_wtile[MAX_CIN_PW],
    acc_t acc[MAC_PD][MAC_PR][MAC_PC])
{
    /* round 15: cyclic factor was hardcoded to 8 (matching the old fixed
     * MAC_PD=8) at all four sites in this file; caught while dropping
     * MAC_PD to 2 -- with a hardcoded 8, bank=(cib+dd) mod 8 stops being
     * compile-time-known for unrolled dd once cib=step*MAC_PD's stride no
     * longer equals the partition factor, reopening a runtime bank-select
     * cost round 5/8 already eliminated. Must always equal MAC_PD. */
    #pragma HLS ARRAY_PARTITION variable=dw_patch complete dim=0
    #pragma HLS ARRAY_PARTITION variable=dw_wtile complete dim=0
    #pragma HLS ARRAY_PARTITION variable=pw_patch cyclic factor=MAC_PD dim=1
    #pragma HLS ARRAY_PARTITION variable=pw_patch complete dim=2
    #pragma HLS ARRAY_PARTITION variable=pw_patch complete dim=3
    #pragma HLS ARRAY_PARTITION variable=pw_wtile cyclic factor=MAC_PD dim=1
    #pragma HLS ARRAY_PARTITION variable=acc      complete dim=0

    /* Per-step gather buffers -- dim=1 (step) is deliberately NOT
     * partitioned/unrolled (UNIFIED below accesses one step at a time,
     * sequentially; only dims 2-4, the 512-wide lane shape, need to be
     * fully parallel-addressable). */
    act_t lane_in_all[MAX_STEPS][MAC_PD][MAC_PR][MAC_PC];
    wt_t  lane_w_all[MAX_STEPS][MAC_PD];
    #pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=2
    #pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=3
    #pragma HLS ARRAY_PARTITION variable=lane_in_all complete dim=4
    #pragma HLS ARRAY_PARTITION variable=lane_w_all  complete dim=2

    /* round 13's fix retained verbatim: DW gets its own doubly-nested
     * compile-time loop (kh, kw each 0..MAX_K-1, both literal bounds) so
     * kh/kw are genuine loop induction variables of directly-bounded
     * loops, not arithmetic derived from a shared flat counter. This
     * phase is NOT the pipelined/unrolled region (that's UNIFIED, below)
     * so a runtime-valued `step = kh*MAX_K+kw` write-index here is cheap
     * ordinary address-counter hardware, not the round-12 fan-out
     * problem (that was specifically about a runtime index feeding a
     * 512-way UNROLLED read). */
    if (op_type == LDESC_OP_DWCONV) {
        if (dw_S == 1) {
            GATHER_ALL_DW_S1: for (int kh = 0; kh < MAX_K; kh++) {
                for (int kw = 0; kw < MAX_K; kw++) {
                    int step = kh * MAX_K + kw;
                    bool valid = (kh < dw_K) && (kw < dw_K);
                    for (int dd = 0; dd < MAC_PD; dd++) {
                        lane_w_all[step][dd] = valid ? dw_wtile[dd][kh][kw] : (wt_t)0;
                        for (int rr = 0; rr < MAC_PR; rr++) {
                            for (int cw = 0; cw < MAC_PC; cw++) {
                                lane_in_all[step][dd][rr][cw] = dw_patch[dd][rr * 1 + kh][cw * 1 + kw];
                            }
                        }
                    }
                }
            }
        } else {
            GATHER_ALL_DW_S2: for (int kh = 0; kh < MAX_K; kh++) {
                for (int kw = 0; kw < MAX_K; kw++) {
                    int step = kh * MAX_K + kw;
                    bool valid = (kh < dw_K) && (kw < dw_K);
                    for (int dd = 0; dd < MAC_PD; dd++) {
                        lane_w_all[step][dd] = valid ? dw_wtile[dd][kh][kw] : (wt_t)0;
                        for (int rr = 0; rr < MAC_PR; rr++) {
                            for (int cw = 0; cw < MAC_PC; cw++) {
                                lane_in_all[step][dd][rr][cw] = dw_patch[dd][rr * 2 + kh][cw * 2 + kw];
                            }
                        }
                    }
                }
            }
        }
    } else {
        GATHER_ALL_PW: for (int step = 0; step < n_steps; step++) {
            int cib = step * MAC_PD;
            for (int dd = 0; dd < MAC_PD; dd++) {
                lane_w_all[step][dd] = pw_wtile[cib + dd];
                for (int rr = 0; rr < MAC_PR; rr++) {
                    for (int cw = 0; cw < MAC_PC; cw++) {
                        lane_in_all[step][dd][rr][cw] = pw_patch[cib + dd][rr][cw];
                    }
                }
            }
        }
    }

    UNIFIED: for (int step = 0; step < n_steps; step++) {
        #pragma HLS PIPELINE II=1
        LANE_D: for (int dd = 0; dd < MAC_PD; dd++) {
            #pragma HLS UNROLL
            LANE_R: for (int rr = 0; rr < MAC_PR; rr++) {
                #pragma HLS UNROLL
                LANE_C: for (int cw = 0; cw < MAC_PC; cw++) {
                    #pragma HLS UNROLL
                    acc[dd][rr][cw] += (acc_t)lane_in_all[step][dd][rr][cw] * (acc_t)lane_w_all[step][dd];
                }
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

    /* A2 fix (2026-08-21, ZHR-92): pw_patch/pw_wtile are fixed at
     * MAX_CIN_PW=32 elements, but staging used to try to fill the WHOLE
     * Cin into them in one pass -- overflowed for every real PW layer
     * (all 26 have cin>32, up to 1152). The reduction itself was never
     * the problem: acc is already a 32-bit accumulator that persists
     * across multiple run_reduce_unified calls within one ot (confirmed:
     * max possible |acc| is ~127*127*1152 =~1.86e7, nowhere near 2**31),
     * and clip_shift only ever ran once per ot already. So the fix is
     * purely in staging -- chunk Cin into MAX_CIN_PW-sized pieces, stage
     * + reduce one chunk at a time, RESET once before the first chunk and
     * WRITEOUT once after the last -- bit-identical to a hypothetical
     * single-pass Cin reduction, not an approximation (verified via a new
     * csim case, cin=1152, against an independent golden reference).
     * Trade-off accepted, not optimized this round: PW's spatial patch
     * used to be staged once per (rt,colt) and reused across every ot;
     * now, with pw_patch too small to hold more than one chunk at a time,
     * it gets re-staged per (ot,chunk) instead -- real added DRAM
     * re-reads, deliberately not addressed this round (get 82/82 running
     * first). */
    const int n_cbase = (d.op_type == LDESC_OP_PWCONV) ? (Cin + MAX_CIN_PW - 1) / MAX_CIN_PW : 1;

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
             * instead of needing pw_patch_full's full width live at once. */
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
                PW_PATCH_HOIST: for (int ci = 0; ci < Cin; ci++) {
                    for (int rr = 0; rr < MAC_PR; rr++) {
                        bool r_valid = rr < r_sz;
                        int oh = rt * MAC_PR + (r_valid ? rr : 0);
                        int base_idx = d.in_off + ci * d.in_ch_stride + oh * W + colt * MAC_PC;
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
                }
            }

            for (int ot = 0; ot < n_ot; ot++) {
                /* known simplification, not yet addressed: DW's receptive-
                 * field reads overlap between spatial lanes (sliding
                 * window), so unlike PW's disjoint Cin banking there is no
                 * clean compile-time-constant bank assignment here without
                 * a real line-buffer/shift-register redesign -- kept
                 * `complete` (register-file, correct but not necessarily
                 * cheap). Dummy/unused when op_type==PWCONV. */
                act_t dw_patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX];
                wt_t  dw_wtile[MAC_PD][MAX_K][MAX_K];
                acc_t dw_btile[MAC_PD];
                #pragma HLS ARRAY_PARTITION variable=dw_patch complete dim=0
                #pragma HLS ARRAY_PARTITION variable=dw_wtile complete dim=0
                #pragma HLS ARRAY_PARTITION variable=dw_btile complete dim=0

                acc_t  pw_bias_val = 0;
                if (d.op_type == LDESC_OP_PWCONV)
                    pw_bias_val = pw_bias_cache[ot];  /* A3 round: was b_base[d.b_off + ot] -- see hoist above */

                int c_sz = MAC_PD;
                int n_f = 1;

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
                 * the MAC reduction, and WRITEOUT now repeat for f=0..fpg-1,
                 * each producing output channel oc = c*fpg+f from the SAME
                 * staged patch with a DIFFERENT weight set. n_f=1 for PW
                 * (fpg always 1 there) makes this loop a no-op wrapper for
                 * the PW path -- unchanged behavior. */
                if (d.op_type == LDESC_OP_DWCONV) {
                    c_sz = (ot == d.n_ch_tiles - 1) ? d.last_ch_tile : MAC_PD;
                    n_f = d.fpg;
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
                    DW_PATCH_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                        bool valid = (cc < c_sz);
                        int c = ot * MAC_PD + (valid ? cc : 0);
                        for (int pr = 0; pr < patch_r; pr++) {
                            int ih = rt * MAC_PR * S - P + pr;
                            for (int pc = 0; pc < patch_c; pc++) {
                                int iw = colt * MAC_PC * S - P + pc;
                                act_t v = 0;
                                if (valid &&
                                    ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                    /* A3 (2026-08-21): (c*Hin+ih)*Win+iw ==
                                     * c*(Hin*Win)+ih*Win+iw exactly -- the
                                     * only difference is Hin*Win is now a
                                     * host-precomputed field read, not a
                                     * hardware multiply of two runtime dims. */
                                    v = in_base[d.in_off + c * d.in_ch_stride + ih * Win + iw];
                                dw_patch[cc][pr][pc] = v;
                            }
                        }
                    }
                }

                for (int f = 0; f < n_f; f++) {
                    if (d.op_type == LDESC_OP_DWCONV) {
                        /* A3 round (2026-08-22, ZHR-92): split from one
                         * combined cc/kh/kw loop into two single-master
                         * loops. Root cause (confirmed via the exact
                         * csynth log message, not inferred): DW_WT_STAGE
                         * used to read BOTH gmem_b (dw_btile, at the cc
                         * level) and gmem_w (dw_wtile, at the kh/kw level)
                         * inside the same pipelined region. HLS's
                         * scheduler couldn't build a continuous request
                         * stream for gmem_w across that misaligned nesting
                         * -- it degraded to one full round-trip per
                         * access ("Unable to schedule bus request
                         * operation ... due to limited memory ports",
                         * achieved II=49, not the target 1). gmem_act's
                         * structurally-identical read pattern (kw-innermost,
                         * monotonic) bursts fine because it's never sharing
                         * a pipelined region with a second bundle; PW_WSTAGE
                         * (gmem_w alone, no second master) reaches II=1 as
                         * the natural control -- same read shape, same
                         * bundle, single-master loop. */
                        DW_BT_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                            bool valid = (cc < c_sz);
                            int c  = ot * MAC_PD + cc;
                            int oc = c * d.fpg + f;
                            int oc_safe = valid ? oc : 0;
                            dw_btile[cc] = valid ? b_base[d.b_off + oc_safe] : (acc_t)0;
                        }
                        /* A3 round (2026-08-22, ZHR-92, followup): kh/kw
                         * bound reverted from the compile-time MAX_K back
                         * to the runtime K -- MAX_K's own justification
                         * ("burst inference needs a compile-time trip
                         * count") stopped holding once this round confirmed
                         * gmem_w never actually bursts even as a
                         * single-master loop at II=1. At MAX_K, this loop
                         * does 2*49=98 iterations (measured 116 cycles);
                         * at K, it's 2*K*K -- 18 for this layer's K=3,
                         * measured ~36 cycles -- saving ~80 cycles/tile
                         * (11.5% of the 694-cycle tile total) for a
                         * property (burst eligibility) that was never
                         * actually achieved. This DOES reopen the
                         * "runtime bound into a complete-partitioned
                         * array" pattern the code review flagged -- but
                         * this specific instance was already identified as
                         * one of the four sites that were "accidentally
                         * safe": GATHER_ALL_DW's own
                         * valid=(kh<dw_K)&&(kw<dw_K) mask zeroes any
                         * kh>=K/kw>=K garbage this loop leaves in
                         * dw_wtile before it can reach the reduction.
                         * DO NOT remove that mask without re-closing this
                         * loop bound at the same time. */
                        DW_WT_STAGE: for (int cc = 0; cc < MAC_PD; cc++) {
                            bool valid = (cc < c_sz);
                            int c  = ot * MAC_PD + cc;
                            int oc = c * d.fpg + f;
                            int oc_safe = valid ? oc : 0;
                            for (int kh = 0; kh < K; kh++)
                                for (int kw = 0; kw < K; kw++) {
                                    dw_wtile[cc][kh][kw] = valid
                                        ? w_base[d.w_off + (oc_safe * K + kh) * K + kw]
                                        : (wt_t)0;
                                }
                        }
                    }

                    acc_t acc[MAC_PD][MAC_PR][MAC_PC];
                    #pragma HLS ARRAY_PARTITION variable=acc complete dim=0
                    RESET: for (int d0 = 0; d0 < MAC_PD; d0++)
                        for (int r0 = 0; r0 < MAC_PR; r0++)
                            for (int c0 = 0; c0 < MAC_PC; c0++) {
                                #pragma HLS UNROLL
                                acc[d0][r0][c0] = 0;
                            }

                    if (d.op_type == LDESC_OP_DWCONV) {
                        act_t pw_patch_dummy[MAX_CIN_PW][MAC_PR][MAC_PC];
                        wt_t  pw_wtile_dummy[MAX_CIN_PW];
                        #pragma HLS ARRAY_PARTITION variable=pw_patch_dummy cyclic factor=MAC_PD dim=1
                        #pragma HLS ARRAY_PARTITION variable=pw_patch_dummy complete dim=2
                        #pragma HLS ARRAY_PARTITION variable=pw_patch_dummy complete dim=3
                        #pragma HLS ARRAY_PARTITION variable=pw_wtile_dummy cyclic factor=MAC_PD dim=1
                        run_reduce_unified(d.op_type, MAX_K * MAX_K,
                                            dw_patch, dw_wtile, K, S,
                                            pw_patch_dummy, pw_wtile_dummy,
                                            acc);
                    } else {
                        /* chunk Cin into MAX_CIN_PW-sized pieces, stage +
                         * reduce one at a time, acc keeps accumulating
                         * across chunks (see the n_cbase comment above for
                         * why this is exact, not approximate). */
                        for (int cbase = 0; cbase < n_cbase; cbase++) {
                            act_t pw_patch[MAX_CIN_PW][MAC_PR][MAC_PC];
                            wt_t  pw_wtile[MAX_CIN_PW];
                            #pragma HLS ARRAY_PARTITION variable=pw_patch cyclic factor=MAC_PD dim=1
                            #pragma HLS ARRAY_PARTITION variable=pw_patch complete dim=2
                            #pragma HLS ARRAY_PARTITION variable=pw_patch complete dim=3
                            #pragma HLS ARRAY_PARTITION variable=pw_wtile cyclic factor=MAC_PD dim=1

                            int c0 = cbase * MAX_CIN_PW;
                            PW_STAGE: for (int ci = 0; ci < MAX_CIN_PW; ci++) {
                                bool valid = (c0 + ci) < Cin;
                                /* A3 round (2026-08-22, ZHR-92, code-review
                                 * followup): rr/cw bound moved from the
                                 * runtime r_sz/col_sz to the compile-time
                                 * MAC_PR/MAC_PC -- pw_patch IS
                                 * complete-partitioned (dims 2,3), so this
                                 * site genuinely had the register-write
                                 * hazard; it was safe only because
                                 * WRITEOUT_PW's own rr<r_sz check discards
                                 * whatever garbage landed past the real
                                 * tile edge. */
                                for (int rr = 0; rr < MAC_PR; rr++) {
                                    for (int cw = 0; cw < MAC_PC; cw++) {
                                        bool rc_valid = valid && (rr < r_sz) && (cw < col_sz);
                                        /* A3 round 2: was a DRAM read
                                         * (in_base[d.in_off + (c0+ci)*d.in_ch_stride
                                         * + oh*W + ow]), re-issued once per
                                         * (rt,colt,ot,cbase) -- now an
                                         * on-chip cache read, populated once
                                         * per (rt,colt) by PW_PATCH_HOIST
                                         * above. */
                                        pw_patch[ci][rr][cw] = rc_valid
                                            ? pw_patch_full[c0 + ci][rr][cw]
                                            : (act_t)0;
                                    }
                                }
                            }
                            PW_WSTAGE: for (int ci = 0; ci < MAX_CIN_PW; ci++)
                                /* A3 round (2026-08-22, ZHR-92): reverted back
                                 * to a direct DRAM read, re-fetched once per
                                 * (rt,colt,ot,cbase) -- see the pw_weight_cache
                                 * revert note above the hoist declaration. */
                                pw_wtile[ci] = ((c0 + ci) < Cin)
                                    ? w_base[d.w_off + ot * Cin + c0 + ci] : (wt_t)0;

                            int remaining = Cin - c0;
                            int this_chunk = (remaining < MAX_CIN_PW) ? remaining : MAX_CIN_PW;
                            int n_steps = (this_chunk + MAC_PD - 1) / MAC_PD;
                            run_reduce_unified(d.op_type, n_steps,
                                                dw_patch, dw_wtile, K, S,
                                                pw_patch, pw_wtile,
                                                acc);
                        }
                    }

                    /* A3 round 2 (2026-08-21, ZHR-92): a second attempt at
                     * hoisting/lookup-table-izing the WRITEOUT address
                     * arithmetic was TRIED and MEASURED WORSE (DSP
                     * 113->118, LUT 42616->42863) -- reverted to this
                     * simpler form, which is the actual measured-better
                     * state. Left as a cautionary note, not silently
                     * dropped: the lookup-table machinery's own cost
                     * (extra adders/registers building rr_row_off/dd_off
                     * each WRITEOUT call) exceeded whatever it saved,
                     * meaning HLS was likely already sharing/reusing the
                     * original per-idx multiply hardware across pipeline
                     * iterations reasonably well -- confirms this is where
                     * the address-arithmetic optimization line stops
                     * being worth pursuing further (per the round's own
                     * stop-loss criterion), not a bug in the attempt. */
                    if (d.op_type == LDESC_OP_DWCONV) {
                        WRITEOUT_DW: for (int idx = 0; idx < MAC_PD * MAC_PR * MAC_PC; idx++) {
                            #pragma HLS PIPELINE II=1
                            int dd = idx / (MAC_PR * MAC_PC);
                            int rr = (idx / MAC_PC) % MAC_PR;
                            int cw = idx % MAC_PC;
                            if (dd >= c_sz || rr >= r_sz || cw >= col_sz) continue;
                            int c  = ot * MAC_PD + dd;
                            int oc = c * d.fpg + f;
                            int oh = rt * MAC_PR + rr;
                            int ow = colt * MAC_PC + cw;
                            int shift = d.use_shift_table ? (int)w_base[d.shift_off + oc] : d.out_shift;
                            out_base[d.out_off + oc * d.out_ch_stride + oh * d.w_out + ow] =
                                (act_t)clip_shift(acc[dd][rr][cw] + dw_btile[dd], shift);
                        }
                    } else {
                        WRITEOUT_PW: for (int idx = 0; idx < MAC_PR * MAC_PC; idx++) {
                            #pragma HLS PIPELINE II=1
                            int rr = idx / MAC_PC, cw = idx % MAC_PC;
                            if (rr >= r_sz || cw >= col_sz) continue;
                            int oh = rt * MAC_PR + rr, ow = colt * MAC_PC + cw;
                            /* round 15: generic MAC_PD-wide combine (was
                             * hardcoded to 8 terms when MAC_PD was fixed at
                             * 8 -- now a compile-time-bounded unrolled sum so
                             * it stays correct at MAC_PD=2 and any other
                             * future width). */
                            acc_t total = 0;
                            COMBINE: for (int dd = 0; dd < MAC_PD; dd++) {
                                #pragma HLS UNROLL
                                total += acc[dd][rr][cw];
                            }
                            int shift = d.use_shift_table ? (int)w_base[d.shift_off + ot] : d.out_shift;
                            out_base[d.out_off + ot * d.out_ch_stride + oh * d.w_out + ow] =
                                (act_t)clip_shift(total + pw_bias_val, shift);
                        }
                    }
                }
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
