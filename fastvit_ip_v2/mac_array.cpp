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
    return p;
}

static acc_t clip_shift(acc_t acc, int shift)
{
    acc_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return v;
}

/* ---- round 14: the actual MAC application, and ONLY the MAC
 * application, as its own non-inlined, function-pipelined unit. Reads
 * only the already-gathered lane_in/lane_w -- zero runtime addressing of
 * any kind inside it, so round 12/13's dw_S and kh/kw problems can't
 * recur here regardless of what fills lane_in upstream. `#pragma HLS
 * PIPELINE II=1` is at FUNCTION scope (no loop inside after the 512-way
 * unroll collapses to combinational-ish logic), which pipelines the
 * CALL sequence itself for back-to-back invocation, without requiring
 * the CALLER's loop to carry its own PIPELINE annotation. That's the
 * point: round 9/10 failed because the call sat inside a caller-owned
 * PIPELINE'd loop, which forces a dedicated per-caller instance to
 * guarantee that loop's own II. Here neither caller loop (DW's tap nest,
 * PW's step loop, both below) is itself PIPELINE'd -- they're ordinary
 * sequential loops that happen to call a function-pipelined callee --
 * so HLS's normal resource sharing across mutually-exclusive callers
 * (round 11's mechanism) should apply. Not yet proven for two SEPARATE
 * call sites reached via two SEPARATE (if merely non-pipelined) loops in
 * the same function -- round 11 only tested one call site -- so this is
 * this round's first and decisive check, not an assumption. */
static void drive_mac(
    const act_t lane_in[MAC_PD][MAC_PR][MAC_PC],
    const wt_t  lane_w[MAC_PD],
    acc_t       acc[MAC_PD][MAC_PR][MAC_PC])
{
    #pragma HLS INLINE off
    #pragma HLS PIPELINE II=1
    #pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
    #pragma HLS ARRAY_PARTITION variable=lane_w  complete dim=0
    #pragma HLS ARRAY_PARTITION variable=acc     complete dim=0
    LANE_D: for (int dd = 0; dd < MAC_PD; dd++) {
        #pragma HLS UNROLL
        LANE_R: for (int rr = 0; rr < MAC_PR; rr++) {
            #pragma HLS UNROLL
            LANE_C: for (int cw = 0; cw < MAC_PC; cw++) {
                #pragma HLS UNROLL
                acc[dd][rr][cw] += (acc_t)lane_in[dd][rr][cw] * (acc_t)lane_w[dd];
            }
        }
    }
}

/* ---- round 10: genuinely shared 512-MAC pipeline. Round 9 put
 * `#pragma HLS PIPELINE II=1` on each CALLER's own step loop
 * (TAP_KH_TAP_KW, REDUCE) and called a shared-but-not-inlined
 * mac_reduce_step from inside it. Confirmed via the resource hierarchy
 * (ZHR-92): two distinct grp_mac_reduce_step_fu_* units, mac_array_top's
 * DSP unchanged at 1047 -- each caller's own pipeline schedule demanded a
 * dedicated per-cycle-available copy of whatever it called, so nothing
 * was actually shared. The PIPELINE annotation has to live in exactly
 * ONE place, inside the shared function itself.
 *
 * round 10 alone wasn't enough either: run_dwconv/run_pwconv were still
 * two separate top-level functions, each with its OWN (single) call site
 * into this function -- confirmed via the resource hierarchy again (two
 * distinct grp_run_reduce_unified_fu_* units, DSP still 1047). Round 9's
 * diagnosis stands regardless of what the shared callee is named: two
 * separate functions means two scheduling domains, and HLS's default
 * resource sharing doesn't reach across that boundary. Round 11 (see
 * run_layer below) removes run_dwconv/run_pwconv entirely so this
 * function has exactly ONE call site in the whole design.
 *
 * DW and PW's addressing still differs (round 5/9's rejected-(a) reason
 * stands: DW's overlapping sliding window vs PW's disjoint Cin banking
 * can't share one buffer layout), so this function takes BOTH shapes as
 * parameters and branches on op_type ONCE PER STEP, before the 512-lane
 * region -- not once per lane, which round 7 already proved is expensive
 * (Expression LUT 79->16,840 for exactly this class of mistake). DW's
 * invalid-tap case and PW's invalid-last-channel-tile case are both
 * handled by zeroing the WEIGHT for that step (round 8's zero-fill
 * approach), never by skipping/branching inside LANE_D/R/C. */
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

    /* round 13: round 12's `int kh = step / MAX_K, kw = step % MAX_K`
     * was the real problem, one level deeper than the dw_S fix reached.
     * step is a genuine runtime pipeline induction variable (this loop
     * body is shared with PW, whose n_steps is a real runtime value, so
     * the compiler can never prove DW's call always sees exactly 9) --
     * so step/MAX_K and step%MAX_K are real divide/modulo hardware whose
     * result fans out into 512 lane addresses + 8 weight lookups + the
     * valid check, and HLS replicates that decode along the fanout to
     * hold II=1 (confirmed: the `sparsemux_*` cores in round 12's
     * Instance bucket, ~3000+ of them). Round 5, 8, and 12 each cleared
     * a different form of "runtime value inside the 512-lane region"
     * (loop bound, guard, dynamic index); this is a fourth form --
     * an induction variable's *derived* value, still runtime despite
     * looking like it comes from a loop.
     *
     * Fix: DW gets its own doubly-nested compile-time loop (kh, kw each
     * 0..MAX_K-1, both literal bounds) so kh/kw are genuine loop
     * induction variables of directly-bounded loops, not arithmetic
     * derived from a shared flat counter -- the pattern Vitis can
     * actually constant-propagate per pipeline stage. This makes DW's
     * LANE_D/R/C block textually distinct from PW's (duplicated source,
     * not shared) -- unavoidable once DW's iteration can no longer share
     * PW's parameterized-bound step loop. Whether this reintroduces
     * round 9/10's two-separate-pipelines duplication (DSP 512->1024) is
     * this round's first and decisive check, not assumed either way. */
    if (op_type == LDESC_OP_DWCONV) {
        DW_TAP_H: for (int kh = 0; kh < MAX_K; kh++) {
            DW_TAP_W: for (int kw = 0; kw < MAX_K; kw++) {
                /* round 14: no PIPELINE here -- this loop is ordinary
                 * sequential control flow; drive_mac carries its own
                 * pipelining. See drive_mac's header for why. */
                bool valid = (kh < dw_K) && (kw < dw_K);
                act_t lane_in[MAC_PD][MAC_PR][MAC_PC];
                wt_t  lane_w[MAC_PD];
                #pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
                #pragma HLS ARRAY_PARTITION variable=lane_w  complete dim=0

                if (dw_S == 1) {
                    GATHER_DW_D_S1: for (int dd = 0; dd < MAC_PD; dd++) {
                        #pragma HLS UNROLL
                        lane_w[dd] = valid ? dw_wtile[dd][kh][kw] : (wt_t)0;
                        GATHER_DW_R_S1: for (int rr = 0; rr < MAC_PR; rr++) {
                            #pragma HLS UNROLL
                            GATHER_DW_C_S1: for (int cw = 0; cw < MAC_PC; cw++) {
                                #pragma HLS UNROLL
                                lane_in[dd][rr][cw] = dw_patch[dd][rr * 1 + kh][cw * 1 + kw];
                            }
                        }
                    }
                } else {
                    GATHER_DW_D_S2: for (int dd = 0; dd < MAC_PD; dd++) {
                        #pragma HLS UNROLL
                        lane_w[dd] = valid ? dw_wtile[dd][kh][kw] : (wt_t)0;
                        GATHER_DW_R_S2: for (int rr = 0; rr < MAC_PR; rr++) {
                            #pragma HLS UNROLL
                            GATHER_DW_C_S2: for (int cw = 0; cw < MAC_PC; cw++) {
                                #pragma HLS UNROLL
                                lane_in[dd][rr][cw] = dw_patch[dd][rr * 2 + kh][cw * 2 + kw];
                            }
                        }
                    }
                }

                drive_mac(lane_in, lane_w, acc);
            }
        }
    } else {
        UNIFIED_PW: for (int step = 0; step < n_steps; step++) {
            /* round 14: no PIPELINE here either -- same reasoning. */
            act_t lane_in[MAC_PD][MAC_PR][MAC_PC];
            wt_t  lane_w[MAC_PD];
            #pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
            #pragma HLS ARRAY_PARTITION variable=lane_w  complete dim=0

            int cib = step * MAC_PD;
            GATHER_PW_D: for (int dd = 0; dd < MAC_PD; dd++) {
                #pragma HLS UNROLL
                lane_w[dd] = pw_wtile[cib + dd];
                GATHER_PW_R: for (int rr = 0; rr < MAC_PR; rr++) {
                    #pragma HLS UNROLL
                    GATHER_PW_C: for (int cw = 0; cw < MAC_PC; cw++) {
                        #pragma HLS UNROLL
                        lane_in[dd][rr][cw] = pw_patch[cib + dd][rr][cw];
                    }
                }
            }

            drive_mac(lane_in, lane_w, acc);
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
                       act_t out_base[])
{
    const int Hin = d.h_in, Win = d.w_in;
    const int K = d.k, S = d.stride, P = d.pad;
    const int patch_r = (MAC_PR - 1) * S + K;
    const int patch_c = (MAC_PC - 1) * S + K;
    const int Cin = d.cin, H = d.h_in, W = d.w_in;

    const int n_ot = (d.op_type == LDESC_OP_DWCONV) ? d.n_ch_tiles : d.cout;

    for (int rt = 0; rt < d.n_row_tiles; rt++) {
        int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
        for (int colt = 0; colt < d.n_col_tiles; colt++) {
            int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

            /* PW's spatial patch: op-independent across ot (no reduction
             * over dd here, dd tiles Cin -- see run_reduce_unified), so
             * staged once per (rt,colt), same as round 5-10. Dummy/unused
             * when op_type==DWCONV. */
            act_t pw_patch[MAX_CIN_PW][MAC_PR][MAC_PC];
            #pragma HLS ARRAY_PARTITION variable=pw_patch cyclic factor=MAC_PD dim=1
            #pragma HLS ARRAY_PARTITION variable=pw_patch complete dim=2
            #pragma HLS ARRAY_PARTITION variable=pw_patch complete dim=3
            if (d.op_type == LDESC_OP_PWCONV) {
                PW_STAGE: for (int ci = 0; ci < Cin; ci++) {
                    for (int rr = 0; rr < r_sz; rr++) {
                        int oh = rt * MAC_PR + rr;
                        for (int cw = 0; cw < col_sz; cw++) {
                            int ow = colt * MAC_PC + cw;
                            pw_patch[ci][rr][cw] = in_base[d.in_off + (ci * H + oh) * W + ow];
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

                wt_t   pw_wtile[MAX_CIN_PW];
                acc_t  pw_bias_val = 0;
                #pragma HLS ARRAY_PARTITION variable=pw_wtile cyclic factor=MAC_PD dim=1

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
                    DW_PATCH_STAGE: for (int cc = 0; cc < c_sz; cc++) {
                        int c = ot * MAC_PD + cc;
                        for (int pr = 0; pr < patch_r; pr++) {
                            int ih = rt * MAC_PR * S - P + pr;
                            for (int pc = 0; pc < patch_c; pc++) {
                                int iw = colt * MAC_PC * S - P + pc;
                                act_t v = 0;
                                if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                    v = in_base[d.in_off + (c * Hin + ih) * Win + iw];
                                dw_patch[cc][pr][pc] = v;
                            }
                        }
                    }
                } else {
                    PW_WSTAGE: for (int ci = 0; ci < MAX_CIN_PW; ci++)
                        pw_wtile[ci] = (ci < Cin) ? w_base[d.w_off + ot * Cin + ci] : (wt_t)0;
                    pw_bias_val = b_base[d.b_off + ot];
                }

                for (int f = 0; f < n_f; f++) {
                    if (d.op_type == LDESC_OP_DWCONV) {
                        DW_WT_STAGE: for (int cc = 0; cc < c_sz; cc++) {
                            int c  = ot * MAC_PD + cc;
                            int oc = c * d.fpg + f;
                            dw_btile[cc] = b_base[d.b_off + oc];
                            for (int kh = 0; kh < K; kh++)
                                for (int kw = 0; kw < K; kw++)
                                    dw_wtile[cc][kh][kw] = w_base[d.w_off + (oc * K + kh) * K + kw];
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

                    int n_steps = (d.op_type == LDESC_OP_DWCONV) ? (MAX_K * MAX_K) : d.n_ch_tiles;
                    run_reduce_unified(d.op_type, n_steps,
                                        dw_patch, dw_wtile, K, S,
                                        pw_patch, pw_wtile,
                                        acc);

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
                            out_base[d.out_off + (oc * d.h_out + oh) * d.w_out + ow] =
                                (act_t)clip_shift(acc[dd][rr][cw] + dw_btile[dd], d.out_shift);
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
                            out_base[d.out_off + (ot * d.h_out + oh) * d.w_out + ow] =
                                (act_t)clip_shift(total + pw_bias_val, d.out_shift);
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

/* A2 (2026-08-21): layer_scale -- structurally identical to run_scale
 * (per-channel broadcast multiply), the only difference is where op1
 * comes from: a TRAINED WEIGHT (w_base at w_off, cin values, one per
 * channel) instead of a computed activation (in_base at in2_off). Found
 * during the A2 design pass -- every mlp/fc2 output in the real network
 * feeds a layer_scale Mul (10 instances) before the residual Add, and no
 * A1 test sequence exercised it since none was built. */
static void run_lscale(const LayerDescV2 &d, const act_t in_base[], const wt_t w_base[], act_t out_base[])
{
    const int HW = d.h_in * d.w_in;
    LSCALE_C: for (int c = 0; c < d.cin; c++) {
        wt_t gate = w_base[d.w_off + c];
        LSCALE_HW: for (int i = 0; i < HW; i++) {
            #pragma HLS PIPELINE II=1
            acc_t prod = (acc_t)in_base[d.in_off + c * HW + i] * (acc_t)gate;
            out_base[d.out_off + c * HW + i] = (act_t)clip_shift(prod, d.out_shift);
        }
    }
}

void mac_array_top(
    const LayerDescV2 desc[],
    int n_layers,
    const act_t  in_base[],
    const wt_t   w_base[],
    const acc_t  b_base[],
    act_t        out_base[],
    int          out_written[])
{
    for (int i = 0; i < n_layers; i++) {
        switch (desc[i].op_type) {
            case LDESC_OP_ADD:     run_add(desc[i], in_base, out_base); break;
            case LDESC_OP_GAP:     run_gap(desc[i], in_base, out_base); break;
            case LDESC_OP_RELU:    run_relu(desc[i], in_base, out_base); break;
            case LDESC_OP_SIGMOID: run_sigmoid(desc[i], in_base, out_base); break;
            case LDESC_OP_SCALE:   run_scale(desc[i], in_base, out_base); break;
            case LDESC_OP_GELU:    run_gelu(desc[i], in_base, out_base); break;
            case LDESC_OP_LSCALE:  run_lscale(desc[i], in_base, w_base, out_base); break;
            default:                run_layer(desc[i], in_base, w_base, b_base, out_base); break;
        }
        out_written[i] = 1;
    }
}
