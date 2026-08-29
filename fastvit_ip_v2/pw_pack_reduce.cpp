// pw_pack_reduce.cpp -- see pw_pack_reduce.h. dsp_pack_mul/
// dsp_pack_mul_signed below are reused VERBATIM from
// dsp_pack_array_probe.cpp (ZHR-92, 2026-08-27) -- that core is already
// exhaustively verified (unsigned: 262,144/262,144; signed
// bias-correction: 262,144/262,144, cross-checked in Python and in real
// HLS csim) -- not reinvented, not re-verified here.
#include "pw_pack_reduce.h"

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
    wt4_t w0, wt4_t w1, act5_t a0, act5_t a1,
    acc_t &p00, acc_t &p01, acc_t &p10, acc_t &p11)
{
#pragma HLS INLINE off
    ap_uint<4> w0_u = (ap_uint<4>)(w0 + 8);
    ap_uint<4> w1_u = (ap_uint<4>)(w1 + 8);
    ap_uint<5> a0_u = (ap_uint<5>)(a0 + 16);
    ap_uint<5> a1_u = (ap_uint<5>)(a1 + 16);

    ap_uint<9> u00, u01, u10, u11;
    dsp_pack_mul(w0_u, w1_u, a0_u, a1_u, u00, u01, u10, u11);

    p00 = (acc_t)u00 - 8 * (acc_t)a0_u - 16 * (acc_t)w0_u + 128;
    p01 = (acc_t)u01 - 8 * (acc_t)a1_u - 16 * (acc_t)w0_u + 128;
    p10 = (acc_t)u10 - 8 * (acc_t)a0_u - 16 * (acc_t)w1_u + 128;
    p11 = (acc_t)u11 - 8 * (acc_t)a1_u - 16 * (acc_t)w1_u + 128;
}

void pw_reduce_unpacked(
    int cin, int cout, int last_r, int last_c,
    const wt4_t weight[], const act5_t patch[], acc_t acc_out[])
{
    for (int co = 0; co < cout; co++)
        for (int r = 0; r < MAC_PR; r++)
            for (int c = 0; c < MAC_PC; c++)
                acc_out[(co * MAC_PR + r) * MAC_PC + c] = 0;

    for (int co = 0; co < cout; co++) {
        for (int ci = 0; ci < cin; ci++) {
            wt4_t w = weight[co * cin + ci];
            for (int r = 0; r < MAC_PR; r++) {
                bool rv = r < last_r;
                for (int c = 0; c < MAC_PC; c++) {
                    bool valid = rv && (c < last_c);
                    act5_t a = patch[(r * MAC_PC + c) * cin + ci];
                    acc_t prod = valid ? (acc_t)((acc_t)w * (acc_t)a) : (acc_t)0;
                    acc_out[(co * MAC_PR + r) * MAC_PC + c] += prod;
                }
            }
        }
    }
}

// Pairing choice (concrete, not previously specified): weight side pairs
// (co, co+1); activation side pairs consecutive flat spatial indices
// (2i, 2i+1) in [rr][cw] row-major order -- e.g. (r=0,c=0)&(r=0,c=1),
// (r=0,c=2)&(r=0,c=3), (r=1,c=0)&(r=1,c=1), etc. cout is real-network
// guaranteed even (checked: all 12 distinct real PW cout values are
// even, plan Q3); MAC_PR*MAC_PC=16 is even by construction so the
// spatial pairing never has a leftover singleton.
void pw_reduce_packed(
    int cin, int cout, int last_r, int last_c,
    const wt4_t weight[], const act5_t patch[], acc_t acc_out[])
{
    for (int co = 0; co < cout; co++)
        for (int r = 0; r < MAC_PR; r++)
            for (int c = 0; c < MAC_PC; c++)
                acc_out[(co * MAC_PR + r) * MAC_PC + c] = 0;

    for (int co0 = 0; co0 < cout; co0 += 2) {
        int co1 = co0 + 1;
        for (int ci = 0; ci < cin; ci++) {
            wt4_t w0 = weight[co0 * cin + ci];
            wt4_t w1 = weight[co1 * cin + ci];
            for (int pos0 = 0; pos0 < MAC_PR * MAC_PC; pos0 += 2) {
                int pos1 = pos0 + 1;
                int r0 = pos0 / MAC_PC, c0 = pos0 % MAC_PC;
                int r1 = pos1 / MAC_PC, c1 = pos1 % MAC_PC;
                bool v0 = (r0 < last_r) && (c0 < last_c);
                bool v1 = (r1 < last_r) && (c1 < last_c);
                act5_t a0 = patch[(r0 * MAC_PC + c0) * cin + ci];
                act5_t a1 = patch[(r1 * MAC_PC + c1) * cin + ci];

                acc_t p00, p01, p10, p11;
                dsp_pack_mul_signed(w0, w1, a0, a1, p00, p01, p10, p11);

                if (v0) {
                    acc_out[(co0 * MAC_PR + r0) * MAC_PC + c0] += p00;
                    acc_out[(co1 * MAC_PR + r0) * MAC_PC + c0] += p10;
                }
                if (v1) {
                    acc_out[(co0 * MAC_PR + r1) * MAC_PC + c1] += p01;
                    acc_out[(co1 * MAC_PR + r1) * MAC_PC + c1] += p11;
                }
            }
        }
    }
}
