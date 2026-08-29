// pw_pack_pipeline.cpp -- see pw_pack_pipeline.h. dsp_pack_mul/
// dsp_pack_mul_signed reused VERBATIM from dsp_pack_array_probe.cpp /
// pw_pack_reduce.cpp (already exhaustively verified, 262,144/262,144
// signed combinations) -- not reinvented, not re-verified here.
#include "pw_pack_pipeline.h"

// mirrors mac_array.cpp's own #defines (file-local there, not exposed via
// mac_array.h) -- same values, not redefining the real ones.
#define PW_FLAT_STEPS_PER_CBASE (MAX_CIN_PW / MAC_PD)
#define PW_FLAT_WRITEOUT_ELEMS  (MAC_PR * MAC_PC)

typedef ap_int<5> pkact_t;
typedef ap_int<4> pkwt_t;

static inline pkwt_t pw_pack_trunc_w(wt_t w) { return (pkwt_t)(w >> 4); }
static inline pkact_t pw_pack_trunc_a(act_t a) { return (pkact_t)(a >> 3); }

static void dsp_pack_mul(
    ap_uint<4> w0, ap_uint<4> w1,
    ap_uint<5> a0, ap_uint<5> a1,
    ap_uint<9> &p00, ap_uint<9> &p01, ap_uint<9> &p10, ap_uint<9> &p11
) {
#pragma HLS INLINE off
    ap_uint<25> A = 0;
    A.range(4, 0) = w0;
    A.range(23, 20) = w1;
    ap_uint<18> B = 0;
    B.range(4, 0) = a0;
    B.range(14, 10) = a1;
    ap_uint<43> P;
#pragma HLS BIND_OP variable=P op=mul impl=DSP
    P = A * B;
    p00 = P.range(8, 0);
    p01 = P.range(18, 10);
    p10 = P.range(28, 20);
    p11 = P.range(38, 30);
}

static void dsp_pack_mul_signed(
    pkwt_t w0, pkwt_t w1, pkact_t a0, pkact_t a1,
    acc_t &p00, acc_t &p01, acc_t &p10, acc_t &p11)
{
#pragma HLS INLINE off
    ap_uint<4> w0_u = (ap_uint<4>)(w0 + 8);
    ap_uint<4> w1_u = (ap_uint<4>)(w1 + 8);
    ap_uint<5> a0_u = (ap_uint<5>)(a0 + 16);
    ap_uint<5> a1_u = (ap_uint<5>)(a1 + 16);

    ap_uint<9> u00, u01, u10, u11;
    dsp_pack_mul(w0_u, w1_u, a0_u, a1_u, u00, u01, u10, u11);

    p00 = (acc_t)((acc_t)u00 - 8 * (acc_t)a0_u - 16 * (acc_t)w0_u + 128);
    p01 = (acc_t)((acc_t)u01 - 8 * (acc_t)a1_u - 16 * (acc_t)w0_u + 128);
    p10 = (acc_t)((acc_t)u10 - 8 * (acc_t)a0_u - 16 * (acc_t)w1_u + 128);
    p11 = (acc_t)((acc_t)u11 - 8 * (acc_t)a1_u - 16 * (acc_t)w1_u + 128);
}

static acc_t pw_pack_clip_shift(acc_t acc, int shift)
{
    acc_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return v;
}

void pw_flat_pipeline_packed(
    const LayerDescV2 &d,
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[],
    int rt, int colt, int r_sz, int col_sz)
{
#pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
#pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3

    const int Cin = d.cin;
    const int n_ot_pairs = d.cout / 2;   // caller-guaranteed even (real network: always true)
    const int n_cbase = (Cin + MAX_CIN_PW - 1) / MAX_CIN_PW;
    const int total_iters = n_ot_pairs * (n_cbase * PW_FLAT_STEPS_PER_CBASE + PW_FLAT_WRITEOUT_ELEMS);

    acc_t acc2[2][MAC_PR][MAC_PC];
#pragma HLS ARRAY_PARTITION variable=acc2 complete dim=0

    int k = 0;
    bool in_writeout = false;
    int cbase_idx = 0;
    int ch_off = 0;
    int w_ot0_base = 0;              /* == ot0*Cin, accumulated (ot0 = 2*ot_pair_idx) */
    int w_ot1_base = Cin;            /* == ot1*Cin, ot1 = ot0+1 */
    int ot0_out_ch_base = 0;
    int ot1_out_ch_base = d.out_ch_stride;
    int ot0_idx = 0;
    int ot1_idx = 1;
    int wr_row = 0, wr_col = 0;
    int shift0 = d.out_shift, shift1 = d.out_shift;

    PW_FLAT_PACKED: for (int i = 0; i < total_iters; i++) {
#pragma HLS PIPELINE II=1
        bool reset_acc = (!in_writeout) && (cbase_idx == 0) && (k == 0);

        if (!in_writeout) {
            wt_t w0 = w_base[d.w_off + w_ot0_base + ch_off];
            wt_t w1 = w_base[d.w_off + w_ot1_base + ch_off];
            pkwt_t w0p = pw_pack_trunc_w(w0);
            pkwt_t w1p = pw_pack_trunc_w(w1);

            act_t lane_in[MAC_PR][MAC_PC];
#pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
            for (int rr = 0; rr < MAC_PR; rr++) {
#pragma HLS UNROLL
                for (int cw = 0; cw < MAC_PC; cw++) {
#pragma HLS UNROLL
                    lane_in[rr][cw] = pw_patch_full[ch_off][rr][cw];
                }
            }

            /* 8 spatial pairs covering all 16 (rr,cw) lanes, consecutive
             * flat index (2p, 2p+1) in row-major order -- see header
             * comment for why this pairing, not a different one. */
            for (int p = 0; p < (MAC_PR * MAC_PC) / 2; p++) {
#pragma HLS UNROLL
                int idx0 = 2 * p, idx1 = 2 * p + 1;
                int r0 = idx0 / MAC_PC, c0 = idx0 % MAC_PC;
                int r1 = idx1 / MAC_PC, c1 = idx1 % MAC_PC;
                pkact_t a0 = pw_pack_trunc_a(lane_in[r0][c0]);
                pkact_t a1 = pw_pack_trunc_a(lane_in[r1][c1]);

                acc_t p00, p01, p10, p11;
                dsp_pack_mul_signed(w0p, w1p, a0, a1, p00, p01, p10, p11);

                acc2[0][r0][c0] = reset_acc ? p00 : (acc_t)(acc2[0][r0][c0] + p00);
                acc2[0][r1][c1] = reset_acc ? p01 : (acc_t)(acc2[0][r1][c1] + p01);
                acc2[1][r0][c0] = reset_acc ? p10 : (acc_t)(acc2[1][r0][c0] + p10);
                acc2[1][r1][c1] = reset_acc ? p11 : (acc_t)(acc2[1][r1][c1] + p11);
            }
        } else {
            /* writeout: BOTH paired channels' value at (wr_row,wr_col) in
             * the SAME step -- this is the structural question this
             * round's csynth answers (does this cost II like DW raster's
             * fpg dual-lane did, or not). Scalar store only, no
             * out_burst/FAST_WRITEOUT this round (see header comment). */
            act_t val0 = 0, val1 = 0;
            if (wr_row < r_sz && wr_col < col_sz) {
                acc_t total0 = acc2[0][wr_row][wr_col] + pw_bias_cache[ot0_idx];
                acc_t total1 = acc2[1][wr_row][wr_col] + pw_bias_cache[ot1_idx];
                val0 = (act_t)pw_pack_clip_shift(total0, shift0);
                val1 = (act_t)pw_pack_clip_shift(total1, shift1);
            }
            if (wr_row < r_sz && wr_col < col_sz) {
                int addr0 = d.out_off + ot0_out_ch_base + (rt * MAC_PR + wr_row) * d.w_out + colt * MAC_PC;
                int addr1 = d.out_off + ot1_out_ch_base + (rt * MAC_PR + wr_row) * d.w_out + colt * MAC_PC;
                out_base[addr0 + wr_col] = val0;
                out_base[addr1 + wr_col] = val1;
            }
        }

        int wrap_bound = in_writeout ? PW_FLAT_WRITEOUT_ELEMS : PW_FLAT_STEPS_PER_CBASE;
        if (k == wrap_bound - 1) {
            k = 0;
            if (!in_writeout) {
                if (cbase_idx == n_cbase - 1) {
                    in_writeout = true;
                    shift0 = d.use_shift_table ? (int)pw_shift_cache[ot0_idx] : d.out_shift;
                    shift1 = d.use_shift_table ? (int)pw_shift_cache[ot1_idx] : d.out_shift;
                } else {
                    cbase_idx++;
                    ch_off++;
                }
            } else {
                in_writeout = false;
                cbase_idx = 0;
                ch_off = 0;
                w_ot0_base += 2 * Cin;
                w_ot1_base += 2 * Cin;
                ot0_out_ch_base += 2 * d.out_ch_stride;
                ot1_out_ch_base += 2 * d.out_ch_stride;
                ot0_idx += 2;
                ot1_idx += 2;
                wr_row = 0;
                wr_col = 0;
            }
        } else {
            k++;
            if (!in_writeout) {
                ch_off++;
            } else {
                if (wr_col == MAC_PC - 1) { wr_col = 0; wr_row++; }
                else { wr_col++; }
            }
        }
    }
}

// ZHR-92 DSP-packing Step 3 (2026-08-29): literal-constant copy of the
// body above (cin=32 -> n_cbase=1 exactly, cout=4 -> n_ot_pairs=2,
// theoretical total_iters = n_ot_pairs*(n_cbase*32+16) = 2*48 = 96) --
// separate body, not just a fixed-arg call, so HLS can resolve the trip
// count at compile time (same methodology as Step 1's
// dw_linebuf_probe2_fixed; a fixed LayerDescV2 alone doesn't reliably
// constant-propagate through a struct field the way a literal does).
void pw_flat_pipeline_packed_fixed(
    const wt_t w_base[],
    const act_t pw_patch_full[MAX_CIN][MAC_PR][MAC_PC],
    const acc_t pw_bias_cache[MAX_PW_BIAS_CACHE],
    const wt_t pw_shift_cache[MAX_PW_BIAS_CACHE],
    act_t out_base[])
{
#pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=2
#pragma HLS ARRAY_PARTITION variable=pw_patch_full complete dim=3

    const int CIN = 32, N_OT_PAIRS = 2, N_CBASE = 1;
    const int TOTAL_ITERS = N_OT_PAIRS * (N_CBASE * PW_FLAT_STEPS_PER_CBASE + PW_FLAT_WRITEOUT_ELEMS);
    const int W_OFF = 0, OUT_OFF = 0, OUT_CH_STRIDE = 1, W_OUT = 1;
    const int RT = 0, COLT = 0, R_SZ = 1, COL_SZ = 1;
    const bool USE_SHIFT_TABLE = true;

    acc_t acc2[2][MAC_PR][MAC_PC];
#pragma HLS ARRAY_PARTITION variable=acc2 complete dim=0

    int k = 0;
    bool in_writeout = false;
    int cbase_idx = 0;
    int ch_off = 0;
    int w_ot0_base = 0;
    int w_ot1_base = CIN;
    int ot0_out_ch_base = 0;
    int ot1_out_ch_base = OUT_CH_STRIDE;
    int ot0_idx = 0;
    int ot1_idx = 1;
    int wr_row = 0, wr_col = 0;
    int shift0 = 4, shift1 = 4;

    FIXED_PW_FLAT_PACKED: for (int i = 0; i < TOTAL_ITERS; i++) {
#pragma HLS PIPELINE II=1
        bool reset_acc = (!in_writeout) && (cbase_idx == 0) && (k == 0);

        if (!in_writeout) {
            wt_t w0 = w_base[W_OFF + w_ot0_base + ch_off];
            wt_t w1 = w_base[W_OFF + w_ot1_base + ch_off];
            pkwt_t w0p = pw_pack_trunc_w(w0);
            pkwt_t w1p = pw_pack_trunc_w(w1);

            act_t lane_in[MAC_PR][MAC_PC];
#pragma HLS ARRAY_PARTITION variable=lane_in complete dim=0
            for (int rr = 0; rr < MAC_PR; rr++) {
#pragma HLS UNROLL
                for (int cw = 0; cw < MAC_PC; cw++) {
#pragma HLS UNROLL
                    lane_in[rr][cw] = pw_patch_full[ch_off][rr][cw];
                }
            }

            for (int p = 0; p < (MAC_PR * MAC_PC) / 2; p++) {
#pragma HLS UNROLL
                int idx0 = 2 * p, idx1 = 2 * p + 1;
                int r0 = idx0 / MAC_PC, c0 = idx0 % MAC_PC;
                int r1 = idx1 / MAC_PC, c1 = idx1 % MAC_PC;
                pkact_t a0 = pw_pack_trunc_a(lane_in[r0][c0]);
                pkact_t a1 = pw_pack_trunc_a(lane_in[r1][c1]);

                acc_t p00, p01, p10, p11;
                dsp_pack_mul_signed(w0p, w1p, a0, a1, p00, p01, p10, p11);

                acc2[0][r0][c0] = reset_acc ? p00 : (acc_t)(acc2[0][r0][c0] + p00);
                acc2[0][r1][c1] = reset_acc ? p01 : (acc_t)(acc2[0][r1][c1] + p01);
                acc2[1][r0][c0] = reset_acc ? p10 : (acc_t)(acc2[1][r0][c0] + p10);
                acc2[1][r1][c1] = reset_acc ? p11 : (acc_t)(acc2[1][r1][c1] + p11);
            }
        } else {
            act_t val0 = 0, val1 = 0;
            if (wr_row < R_SZ && wr_col < COL_SZ) {
                acc_t total0 = acc2[0][wr_row][wr_col] + pw_bias_cache[ot0_idx];
                acc_t total1 = acc2[1][wr_row][wr_col] + pw_bias_cache[ot1_idx];
                val0 = (act_t)pw_pack_clip_shift(total0, shift0);
                val1 = (act_t)pw_pack_clip_shift(total1, shift1);
            }
            if (wr_row < R_SZ && wr_col < COL_SZ) {
                int addr0 = OUT_OFF + ot0_out_ch_base + (RT * MAC_PR + wr_row) * W_OUT + COLT * MAC_PC;
                int addr1 = OUT_OFF + ot1_out_ch_base + (RT * MAC_PR + wr_row) * W_OUT + COLT * MAC_PC;
                out_base[addr0 + wr_col] = val0;
                out_base[addr1 + wr_col] = val1;
            }
        }

        int wrap_bound = in_writeout ? PW_FLAT_WRITEOUT_ELEMS : PW_FLAT_STEPS_PER_CBASE;
        if (k == wrap_bound - 1) {
            k = 0;
            if (!in_writeout) {
                if (cbase_idx == N_CBASE - 1) {
                    in_writeout = true;
                    shift0 = USE_SHIFT_TABLE ? (int)pw_shift_cache[ot0_idx] : 4;
                    shift1 = USE_SHIFT_TABLE ? (int)pw_shift_cache[ot1_idx] : 4;
                } else {
                    cbase_idx++;
                    ch_off++;
                }
            } else {
                in_writeout = false;
                cbase_idx = 0;
                ch_off = 0;
                w_ot0_base += 2 * CIN;
                w_ot1_base += 2 * CIN;
                ot0_out_ch_base += 2 * OUT_CH_STRIDE;
                ot1_out_ch_base += 2 * OUT_CH_STRIDE;
                ot0_idx += 2;
                ot1_idx += 2;
                wr_row = 0;
                wr_col = 0;
            }
        } else {
            k++;
            if (!in_writeout) {
                ch_off++;
            } else {
                if (wr_col == MAC_PC - 1) { wr_col = 0; wr_row++; }
                else { wr_col++; }
            }
        }
    }
}
