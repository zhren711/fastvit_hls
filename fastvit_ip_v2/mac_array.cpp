#include "mac_array.h"

/* Host-side only (see mac_array.h) -- NOT reachable from mac_array_top's
 * call graph, so none of this division is synthesized into hardware. */
MacArrayParams derive_mac_array_params(const LayerDescV2 &d)
{
    MacArrayParams p;
    p.h_out = (d.h_in + 2 * d.pad - d.k) / d.stride + 1;
    p.w_out = (d.w_in + 2 * d.pad - d.k) / d.stride + 1;
    int ch_dim = (d.op_type == LDESC_OP_DWCONV) ? d.cin : d.cout;

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

/* Round 4 (2026-08-16): restructured in response to round 3's finding that
 * ARRAY_PARTITION complete + a flattened UNROLL-factor loop makes HLS
 * synthesize MAC_UNROLL_FACTOR independent pipelined FSMs (one per lane)
 * instead of one shared-control datapath -- literal "copy the loop body N
 * times", not an actual MAC array. Two structural changes, single
 * variable (only MAC_UNROLL_FACTOR=64 is tested this round, matching the
 * prior 64-point measurement 1:1 so the comparison isolates the code
 * structure change):
 *
 *   1. Spatial patch dims are flattened into ONE dimension and partitioned
 *      `cyclic factor=8` (not `complete`) -- only the dimension actually
 *      read by parallel lanes gets banked, the reduction dimension (DW's
 *      kernel taps, PW's Cin) is left un-partitioned so it can live in
 *      BRAM the way the paper's own resource profile (BRAM=105, nonzero)
 *      implies it should.
 *   2. COMPUTE is now an explicit OUTER(PIPELINE)/INNER(UNROLL) nest
 *      instead of one flattened loop with a bare UNROLL factor -- this is
 *      the idiom that makes HLS schedule the unrolled body as ONE shared
 *      pipeline datapath (single FSM controlling MAC_UNROLL_FACTOR
 *      parallel MAC lanes) rather than replicating independent control
 *      per lane. */

/* patch's spatial dim flattened to one axis so a single ARRAY_PARTITION
 * pragma targets exactly "the dimension parallel lanes read from". */
#define DW_PATCH_SPATIAL (PATCH_R_MAX * PATCH_C_MAX)
#define PW_PATCH_SPATIAL (MAC_PR * MAC_PC)

static void run_dwconv(const LayerDescV2 &d,
                        const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                        act_t out_base[])
{
    const int C = d.cin, Hin = d.h_in, Win = d.w_in;
    const int K = d.k, S = d.stride, P = d.pad;
    const int patch_r = (MAC_PR - 1) * S + K;
    const int patch_c = (MAC_PC - 1) * S + K;
    const int TOTAL = MAC_PD * MAC_PR * MAC_PC;          /* 512 */
    const int OUTER_TRIP = TOTAL / MAC_UNROLL_FACTOR;     /* e.g. 512/64=8 */

    for (int ct = 0; ct < d.n_ch_tiles; ct++) {
        int c_sz = (ct == d.n_ch_tiles - 1) ? d.last_ch_tile : MAC_PD;
        for (int rt = 0; rt < d.n_row_tiles; rt++) {
            int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
            for (int colt = 0; colt < d.n_col_tiles; colt++) {
                int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

                act_t patch[MAC_PD][DW_PATCH_SPATIAL];  /* per-channel receptive field, spatial flattened */
                wt_t  wtile[MAC_PD][MAX_K][MAX_K];       /* reduction operands -- small, kept complete */
                acc_t btile[MAC_PD];
                #pragma HLS ARRAY_PARTITION variable=patch  cyclic factor=8 dim=2
                #pragma HLS ARRAY_PARTITION variable=patch  complete dim=1
                #pragma HLS ARRAY_PARTITION variable=wtile  complete dim=0
                #pragma HLS ARRAY_PARTITION variable=btile  complete dim=0

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
                            patch[cc][pr * PATCH_C_MAX + pc] = v;
                        }
                    }
                }

                OUTER: for (int t = 0; t < OUTER_TRIP; t++) {
                    #pragma HLS PIPELINE II=1
                    INNER: for (int p = 0; p < MAC_UNROLL_FACTOR; p++) {
                        #pragma HLS UNROLL
                        int idx = t * MAC_UNROLL_FACTOR + p;
                        int cc = idx / (MAC_PR * MAC_PC);
                        int rr = (idx / MAC_PC) % MAC_PR;
                        int cw = idx % MAC_PC;
                        if (cc >= c_sz || rr >= r_sz || cw >= col_sz) continue;

                        int c  = ct * MAC_PD + cc;
                        int oh = rt * MAC_PR + rr;
                        int ow = colt * MAC_PC + cw;

                        acc_t acc = btile[cc];
                        for (int kh = 0; kh < K; kh++)
                            for (int kw = 0; kw < K; kw++)
                                acc += (acc_t)patch[cc][(rr * S + kh) * PATCH_C_MAX + (cw * S + kw)] *
                                       (acc_t)wtile[cc][kh][kw];

                        out_base[d.out_off + (c * d.h_out + oh) * d.w_out + ow] =
                            (act_t)clip_shift(acc, d.out_shift);
                    }
                }
            }
        }
    }
}

static void run_pwconv(const LayerDescV2 &d,
                        const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                        act_t out_base[])
{
    const int Cin = d.cin, H = d.h_in, W = d.w_in;  /* PW: k=1,stride=1,pad=0 -> h_out=h_in, w_out=w_in */
    const int TOTAL = MAC_PD * MAC_PR * MAC_PC;
    const int OUTER_TRIP = TOTAL / MAC_UNROLL_FACTOR;

    for (int ct = 0; ct < d.n_ch_tiles; ct++) {
        int c_sz = (ct == d.n_ch_tiles - 1) ? d.last_ch_tile : MAC_PD;
        for (int rt = 0; rt < d.n_row_tiles; rt++) {
            int r_sz = (rt == d.n_row_tiles - 1) ? d.last_row_tile : MAC_PR;
            for (int colt = 0; colt < d.n_col_tiles; colt++) {
                int col_sz = (colt == d.n_col_tiles - 1) ? d.last_col_tile : MAC_PC;

                /* PW reduces across the full input-channel depth (unlike
                 * DW's per-channel receptive field). Cin (the reduction
                 * dim, walked sequentially by each lane) is left
                 * UN-partitioned -- BRAM-natural -- per the 2026-08-16
                 * direction; only the spatial dim parallel lanes actually
                 * read simultaneously gets banked. */
                act_t patch[MAX_CIN_PW][PW_PATCH_SPATIAL];
                wt_t  wtile[MAC_PD][MAX_CIN_PW];
                acc_t btile[MAC_PD];
                #pragma HLS ARRAY_PARTITION variable=patch  cyclic factor=8 dim=2
                #pragma HLS ARRAY_PARTITION variable=wtile  complete dim=0
                #pragma HLS ARRAY_PARTITION variable=btile  complete dim=0

                STAGE: for (int ci = 0; ci < Cin; ci++) {
                    for (int rr = 0; rr < r_sz; rr++) {
                        int oh = rt * MAC_PR + rr;
                        for (int cw = 0; cw < col_sz; cw++) {
                            int ow = colt * MAC_PC + cw;
                            patch[ci][rr * MAC_PC + cw] = in_base[d.in_off + (ci * H + oh) * W + ow];
                        }
                    }
                }
                for (int cc = 0; cc < c_sz; cc++) {
                    int co = ct * MAC_PD + cc;
                    btile[cc] = b_base[d.b_off + co];
                    for (int ci = 0; ci < Cin; ci++)
                        wtile[cc][ci] = w_base[d.w_off + co * Cin + ci];
                }

                OUTER: for (int t = 0; t < OUTER_TRIP; t++) {
                    #pragma HLS PIPELINE II=1
                    INNER: for (int p = 0; p < MAC_UNROLL_FACTOR; p++) {
                        #pragma HLS UNROLL
                        int idx = t * MAC_UNROLL_FACTOR + p;
                        int cc = idx / (MAC_PR * MAC_PC);
                        int rr = (idx / MAC_PC) % MAC_PR;
                        int cw = idx % MAC_PC;
                        if (cc >= c_sz || rr >= r_sz || cw >= col_sz) continue;

                        int co = ct * MAC_PD + cc;
                        int oh = rt * MAC_PR + rr;
                        int ow = colt * MAC_PC + cw;

                        acc_t acc = btile[cc];
                        for (int ci = 0; ci < Cin; ci++)
                            acc += (acc_t)patch[ci][rr * MAC_PC + cw] * (acc_t)wtile[cc][ci];

                        out_base[d.out_off + (co * d.h_out + oh) * d.w_out + ow] =
                            (act_t)clip_shift(acc, d.out_shift);
                    }
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
