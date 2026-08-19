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

/* ---- round 9: the shared 512-MAC compute step. DW and PW each build a
 * canonical MAC_PD x MAC_PR x MAC_PC "this cycle's operands" view from
 * their own (unchanged, op-specific) patch/wtile addressing, then both
 * call this SAME function -- that's the only thing that makes it one
 * physical 512-MAC engine instead of two. `#pragma HLS INLINE off` is
 * load-bearing: without it Vitis expands a private copy of this loop into
 * each call site (textually identical to what round 5-8 already had) and
 * the merge accomplishes nothing. Weight only depends on dd (broadcasts
 * across rr,cw) for both ops, so w[MAC_PD] is a sufficient shared
 * interface without forcing DW and PW onto a common patch layout -- see
 * ZHR-63/ZHR-92's merge-design discussion for why (a) a shared staging
 * buffer was rejected (S=2 DW bank conflicts under PW's cyclic scheme)
 * in favor of (b) shared compute only. */
static void mac_reduce_step(
    const act_t p[MAC_PD][MAC_PR][MAC_PC],
    const wt_t  w[MAC_PD],
    acc_t       acc[MAC_PD][MAC_PR][MAC_PC])
{
    #pragma HLS INLINE off
    #pragma HLS ARRAY_PARTITION variable=p   complete dim=0
    #pragma HLS ARRAY_PARTITION variable=w   complete dim=0
    #pragma HLS ARRAY_PARTITION variable=acc complete dim=0
    LANE_D: for (int dd = 0; dd < MAC_PD; dd++) {
        #pragma HLS UNROLL
        LANE_R: for (int rr = 0; rr < MAC_PR; rr++) {
            #pragma HLS UNROLL
            LANE_C: for (int cw = 0; cw < MAC_PC; cw++) {
                #pragma HLS UNROLL
                acc[dd][rr][cw] += (acc_t)p[dd][rr][cw] * (acc_t)w[dd];
            }
        }
    }
}

/* ---- DW: pd=8 channel-parallel, pr x pc=64 spatial-parallel, K*K taps
 * time-multiplexed (paper's literal 512-physical-MAC geometry). The tap
 * loop's bound is the compile-time MAX_K, not the runtime field K -- an
 * out-of-range tap is skipped with a runtime `continue` INSIDE the loop
 * body, never by varying the loop's own trip count -- this is what lets
 * PIPELINE actually engage (bug 1). Address arithmetic lives only in
 * STAGE and WRITEOUT, never inside the pipelined tap loop (bug 1's
 * partner issue: per round-3/4, replicating a runtime multiply per lane
 * once pipelining worked would have cost 128 extra 32-bit multipliers). */
static void run_dwconv(const LayerDescV2 &d,
                        const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                        act_t out_base[])
{
    const int Hin = d.h_in, Win = d.w_in;
    const int K = d.k, S = d.stride, P = d.pad;
    const int patch_r = (MAC_PR - 1) * S + K;
    const int patch_c = (MAC_PC - 1) * S + K;

    for (int ct = 0; ct < d.n_ch_tiles; ct++) {
        int c_sz = (ct == d.n_ch_tiles - 1) ? d.last_ch_tile : MAC_PD;
        for (int rt = 0; rt < d.n_row_tiles; rt++) {
            int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
            for (int colt = 0; colt < d.n_col_tiles; colt++) {
                int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

                /* known simplification, not yet addressed this round: DW's
                 * receptive-field reads overlap between spatial lanes
                 * (sliding window), so unlike PW's disjoint Cin banking,
                 * there is no clean compile-time-constant bank assignment
                 * here without a real line-buffer/shift-register redesign
                 * -- kept `complete` (register-file, correct but not
                 * necessarily cheap) rather than guessing at a cyclic
                 * factor that wouldn't actually avoid runtime address
                 * decoding for the kh/kw-dependent, stride-scaled offset. */
                act_t patch[MAC_PD][PATCH_R_MAX][PATCH_C_MAX];
                wt_t  wtile[MAC_PD][MAX_K][MAX_K];
                acc_t btile[MAC_PD];
                acc_t acc[MAC_PD][MAC_PR][MAC_PC];
                #pragma HLS ARRAY_PARTITION variable=patch complete dim=0
                #pragma HLS ARRAY_PARTITION variable=wtile complete dim=0
                #pragma HLS ARRAY_PARTITION variable=btile complete dim=0
                #pragma HLS ARRAY_PARTITION variable=acc   complete dim=0

                STAGE: for (int cc = 0; cc < c_sz; cc++) {
                    int c = ct * MAC_PD + cc;
                    btile[cc] = b_base[d.b_off + c];
                    for (int kh = 0; kh < K; kh++)
                        for (int kw = 0; kw < K; kw++)
                            wtile[cc][kh][kw] = w_base[d.w_off + (c * K + kh) * K + kw];
                    for (int pr = 0; pr < patch_r; pr++) {
                        int ih = rt * MAC_PR * S - P + pr;
                        for (int pc = 0; pc < patch_c; pc++) {
                            int iw = colt * MAC_PC * S - P + pc;
                            act_t v = 0;
                            if (ih >= 0 && ih < Hin && iw >= 0 && iw < Win)
                                v = in_base[d.in_off + (c * Hin + ih) * Win + iw];
                            patch[cc][pr][pc] = v;
                        }
                    }
                }

                RESET: for (int d0 = 0; d0 < MAC_PD; d0++)
                    for (int r0 = 0; r0 < MAC_PR; r0++)
                        for (int c0 = 0; c0 < MAC_PC; c0++) {
                            #pragma HLS UNROLL
                            acc[d0][r0][c0] = 0;
                        }

                TAP_KH: for (int kh = 0; kh < MAX_K; kh++) {
                    TAP_KW: for (int kw = 0; kw < MAX_K; kw++) {
                        #pragma HLS PIPELINE II=1
                        if (kh >= K || kw >= K) continue;
                        act_t p_gather[MAC_PD][MAC_PR][MAC_PC];
                        wt_t  w_gather[MAC_PD];
                        #pragma HLS ARRAY_PARTITION variable=p_gather complete dim=0
                        #pragma HLS ARRAY_PARTITION variable=w_gather complete dim=0
                        GATHER_D: for (int dd = 0; dd < MAC_PD; dd++) {
                            #pragma HLS UNROLL
                            w_gather[dd] = wtile[dd][kh][kw];
                            GATHER_R: for (int rr = 0; rr < MAC_PR; rr++) {
                                #pragma HLS UNROLL
                                GATHER_C: for (int cw = 0; cw < MAC_PC; cw++) {
                                    #pragma HLS UNROLL
                                    p_gather[dd][rr][cw] = patch[dd][rr * S + kh][cw * S + kw];
                                }
                            }
                        }
                        mac_reduce_step(p_gather, w_gather, acc);
                    }
                }

                WRITEOUT: for (int idx = 0; idx < MAC_PD * MAC_PR * MAC_PC; idx++) {
                    #pragma HLS PIPELINE II=1
                    int dd = idx / (MAC_PR * MAC_PC);
                    int rr = (idx / MAC_PC) % MAC_PR;
                    int cw = idx % MAC_PC;
                    if (dd >= c_sz || rr >= r_sz || cw >= col_sz) continue;
                    int c  = ct * MAC_PD + dd;
                    int oh = rt * MAC_PR + rr;
                    int ow = colt * MAC_PC + cw;
                    out_base[d.out_off + (c * d.h_out + oh) * d.w_out + ow] =
                        (act_t)clip_shift(acc[dd][rr][cw] + btile[dd], d.out_shift);
                }
            }
        }
    }
}

/* ---- PW: pd=8 tiles Cin (the reduction axis), pr x pc=64 is the output
 * spatial tile computed in parallel every cycle. Output channels are
 * processed ONE AT A TIME (sequential `co` loop, not tiled) -- the 512
 * physical MACs are reused across all Cout channels, not replicated
 * Cout/8-wide the way round 3/4 did. patch's Cin dimension is cyclically
 * partitioned with factor=MAC_PD: since the reduction loop always steps
 * `cib` by exactly MAC_PD, `cib+d` (d compile-time 0..7) lands in bank
 * `d` -- a COMPILE-TIME-KNOWN bank for every unrolled lane, so no runtime
 * address decoder is needed at all (the fix for bug 2, applied only
 * where the access pattern is actually this clean -- see DW's patch
 * above for the case where it isn't). */
static void run_pwconv(const LayerDescV2 &d,
                        const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                        act_t out_base[])
{
    const int Cin = d.cin, H = d.h_in, W = d.w_in;  /* PW: k=1,stride=1,pad=0 -> h_out=h_in, w_out=w_in */

    for (int rt = 0; rt < d.n_row_tiles; rt++) {
        int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
        for (int colt = 0; colt < d.n_col_tiles; colt++) {
            int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

            act_t patch[MAX_CIN_PW][MAC_PR][MAC_PC];
            #pragma HLS ARRAY_PARTITION variable=patch cyclic factor=8 dim=1
            #pragma HLS ARRAY_PARTITION variable=patch complete dim=2
            #pragma HLS ARRAY_PARTITION variable=patch complete dim=3

            STAGE: for (int ci = 0; ci < Cin; ci++) {
                for (int rr = 0; rr < r_sz; rr++) {
                    int oh = rt * MAC_PR + rr;
                    for (int cw = 0; cw < col_sz; cw++) {
                        int ow = colt * MAC_PC + cw;
                        patch[ci][rr][cw] = in_base[d.in_off + (ci * H + oh) * W + ow];
                    }
                }
            }

            CH_LOOP: for (int co = 0; co < d.cout; co++) {
                wt_t  wtile[MAX_CIN_PW];
                /* round 7: give each dd its own accumulator (512 total,
                 * mirroring DW's acc[MAC_PD][MAC_PR][MAC_PC] exactly) instead
                 * of round 6's shared acc[rr][cw] + separate psum[] tree.
                 * round 6 fixed REDUCE's II by moving the 8-way sum off the
                 * carried-dependency path, but splitting the multiply from
                 * its accumulate broke Vitis's a*b+=acc -> DSP48-mac-adder
                 * pattern match, so all 512 multiplies fell back to LUT
                 * fabric (measured: pwconv DSP 524->12, LUT 40,791->66,453).
                 * Per-dd accumulation keeps `acc[dd][rr][cw] += a*b` intact
                 * -- the exact form DW already proves binds to mac_muladd/
                 * dsp_slice at II=1 -- and pushes the 8-way combine out of
                 * the pipelined region entirely, into WRITEOUT (executed
                 * once per output channel, not once per ct iteration, so it
                 * carries no loop-carried dependency to violate II=1 there
                 * either). Costs 7*MAC_PR*MAC_PC=448 extra acc_t registers
                 * (~14k FF) versus round 5/6. */
                acc_t acc[MAC_PD][MAC_PR][MAC_PC];
                #pragma HLS ARRAY_PARTITION variable=wtile cyclic factor=8 dim=1
                #pragma HLS ARRAY_PARTITION variable=acc   complete dim=0

                /* round 8: the last (possibly partial) channel tile used to
                 * be guarded with `if (dd >= chunk_sz) continue` INSIDE the
                 * unrolled LANE_D loop -- a runtime-valued (chunk_sz derives
                 * from d.last_ch_tile) comparison replicated once per
                 * physical lane/accumulator, same class of error as ZHR-92's
                 * "runtime value inside an UNROLLed region" even though it's
                 * a guard, not a loop bound. Zero-padding wtile out to the
                 * full MAX_CIN_PW here (staging, not unrolled -- a plain
                 * Cin<=32-iteration sequential loop) makes every out-of-
                 * range cib+dd multiply-by-zero and harmless, so the guard
                 * can be deleted from REDUCE entirely instead of replicated
                 * 512 times. */
                WSTAGE: for (int ci = 0; ci < MAX_CIN_PW; ci++)
                    wtile[ci] = (ci < Cin) ? w_base[d.w_off + co * Cin + ci] : (wt_t)0;
                acc_t bias_val = b_base[d.b_off + co];

                RESET: for (int d0 = 0; d0 < MAC_PD; d0++)
                    for (int r0 = 0; r0 < MAC_PR; r0++)
                        for (int c0 = 0; c0 < MAC_PC; c0++) {
                            #pragma HLS UNROLL
                            acc[d0][r0][c0] = 0;
                        }

                REDUCE: for (int ct = 0; ct < d.n_ch_tiles; ct++) {
                    #pragma HLS PIPELINE II=1
                    int cib = ct * MAC_PD;
                    act_t p_gather[MAC_PD][MAC_PR][MAC_PC];
                    wt_t  w_gather[MAC_PD];
                    #pragma HLS ARRAY_PARTITION variable=p_gather complete dim=0
                    #pragma HLS ARRAY_PARTITION variable=w_gather complete dim=0
                    GATHER_D: for (int dd = 0; dd < MAC_PD; dd++) {
                        #pragma HLS UNROLL
                        w_gather[dd] = wtile[cib + dd];
                        GATHER_R: for (int rr = 0; rr < MAC_PR; rr++) {
                            #pragma HLS UNROLL
                            GATHER_C: for (int cw = 0; cw < MAC_PC; cw++) {
                                #pragma HLS UNROLL
                                p_gather[dd][rr][cw] = patch[cib + dd][rr][cw];
                            }
                        }
                    }
                    mac_reduce_step(p_gather, w_gather, acc);
                }

                WRITEOUT: for (int idx = 0; idx < MAC_PR * MAC_PC; idx++) {
                    #pragma HLS PIPELINE II=1
                    int rr = idx / MAC_PC, cw = idx % MAC_PC;
                    if (rr >= r_sz || cw >= col_sz) continue;
                    int oh = rt * MAC_PR + rr, ow = colt * MAC_PC + cw;
                    acc_t s0 = acc[0][rr][cw] + acc[1][rr][cw];
                    acc_t s1 = acc[2][rr][cw] + acc[3][rr][cw];
                    acc_t s2 = acc[4][rr][cw] + acc[5][rr][cw];
                    acc_t s3 = acc[6][rr][cw] + acc[7][rr][cw];
                    acc_t total = (s0 + s1) + (s2 + s3);
                    out_base[d.out_off + (co * d.h_out + oh) * d.w_out + ow] =
                        (act_t)clip_shift(total + bias_val, d.out_shift);
                }
            }
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
        if (desc[i].op_type == LDESC_OP_DWCONV)
            run_dwconv(desc[i], in_base, w_base, b_base, out_base);
        else
            run_pwconv(desc[i], in_base, w_base, b_base, out_base);
        out_written[i] = 1;
    }
}
