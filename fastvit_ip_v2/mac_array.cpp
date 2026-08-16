#include "mac_array.h"

/* Independently re-derived by tools/verify_mac_array_mapping.py -- the two
 * implementations must never be allowed to silently drift (ZHR-91 row 6). */
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

/* One pass of the 8x8x8 time-multiplexed array over a single (row-tile,
 * col-tile, ch-tile) output block. Each PE in the MAC_PR x MAC_PC x MAC_PD
 * grid owns exactly one (row, col, channel) output element and walks its
 * own reduction (kernel taps for DW, input channels for PW) independently
 * -- a real synthesized version pipelines/parallelizes this loop nest with
 * HLS pragmas; this PoC leaves it a plain loop nest since Phase A's job
 * this round is proving the *structure* (tiling, descriptor-driven
 * dispatch, self-verified writeback) is right, not hitting a timing
 * target -- that's Phase D's job (CLAUDE.md), evaluated fresh once this
 * architecture exists to time. */
static void run_dwconv(const LayerDescV2 &d, const MacArrayParams &p,
                        const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                        act_t out_base[])
{
    const int C = d.cin, Hin = d.h_in, Win = d.w_in;
    const int K = d.k, S = d.stride, P = d.pad;

    for (int ct = 0; ct < p.n_ch_tiles; ct++) {
        int c_sz = (ct == p.n_ch_tiles - 1) ? p.last_ch_tile : MAC_PD;
        for (int rt = 0; rt < p.n_row_tiles; rt++) {
            int r_sz = (rt == p.n_row_tiles - 1) ? p.last_row_tile : MAC_PR;
            for (int colt = 0; colt < p.n_col_tiles; colt++) {
                int col_sz = (colt == p.n_col_tiles - 1) ? p.last_col_tile : MAC_PC;

                for (int cc = 0; cc < c_sz; cc++) {
                    int c = ct * MAC_PD + cc;
                    for (int rr = 0; rr < r_sz; rr++) {
                        int oh = rt * MAC_PR + rr;
                        for (int cw = 0; cw < col_sz; cw++) {
                            int ow = colt * MAC_PC + cw;

                            acc_t acc = b_base[d.b_off + c];
                            for (int kh = 0; kh < K; kh++) {
                                int ih = oh * S - P + kh;
                                if (ih < 0 || ih >= Hin) continue;
                                for (int kw = 0; kw < K; kw++) {
                                    int iw = ow * S - P + kw;
                                    if (iw < 0 || iw >= Win) continue;
                                    act_t x = in_base[d.in_off + (c * Hin + ih) * Win + iw];
                                    wt_t  w = w_base[d.w_off + (c * K + kh) * K + kw];
                                    acc += (acc_t)x * (acc_t)w;
                                }
                            }
                            out_base[d.out_off + (c * p.h_out + oh) * p.w_out + ow] =
                                (act_t)clip_shift(acc, d.out_shift);
                        }
                    }
                }
            }
        }
    }
}

static void run_pwconv(const LayerDescV2 &d, const MacArrayParams &p,
                        const act_t in_base[], const wt_t w_base[], const acc_t b_base[],
                        act_t out_base[])
{
    const int Cin = d.cin, H = d.h_in, W = d.w_in;  /* PW: k=1,stride=1,pad=0 -> h_out=h_in, w_out=w_in */

    for (int ct = 0; ct < p.n_ch_tiles; ct++) {
        int c_sz = (ct == p.n_ch_tiles - 1) ? p.last_ch_tile : MAC_PD;
        for (int rt = 0; rt < p.n_row_tiles; rt++) {
            int r_sz = (rt == p.n_row_tiles - 1) ? p.last_row_tile : MAC_PR;
            for (int colt = 0; colt < p.n_col_tiles; colt++) {
                int col_sz = (colt == p.n_col_tiles - 1) ? p.last_col_tile : MAC_PC;

                for (int cc = 0; cc < c_sz; cc++) {
                    int co = ct * MAC_PD + cc;
                    for (int rr = 0; rr < r_sz; rr++) {
                        int oh = rt * MAC_PR + rr;
                        for (int cw = 0; cw < col_sz; cw++) {
                            int ow = colt * MAC_PC + cw;

                            acc_t acc = b_base[d.b_off + co];
                            for (int ci = 0; ci < Cin; ci++) {
                                act_t x = in_base[d.in_off + (ci * H + oh) * W + ow];
                                wt_t  w = w_base[d.w_off + co * Cin + ci];
                                acc += (acc_t)x * (acc_t)w;
                            }
                            out_base[d.out_off + (co * p.h_out + oh) * p.w_out + ow] =
                                (act_t)clip_shift(acc, d.out_shift);
                        }
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
        MacArrayParams p = derive_mac_array_params(desc[i]);
        if (desc[i].op_type == LDESC_OP_DWCONV)
            run_dwconv(desc[i], p, in_base, w_base, b_base, out_base);
        else
            run_pwconv(desc[i], p, in_base, w_base, b_base, out_base);
        out_written[i] = 1;
    }
}
