/*============================================================
 * conv_worker.cpp — ported from conv_ip/conv_ip.cpp v2, unchanged
 * compute/tiling logic; only feat_in/feat_out access is rewritten
 * against packed pack_t ports via read_byte()/packed word writes.
 *============================================================*/

#include "conv_worker.h"

static act_t in_buf [CONV_TN][CONV_IN_TILE_H][CONV_IN_TILE_W];
static wt_t  wt_buf [CONV_TM][CONV_TN][CONV_K][CONV_K];
static acc_t out_buf[CONV_TM][CONV_TR][CONV_TC];

static inline act_t conv_quant_act(acc_t v, int shift, int relu) {
#pragma HLS INLINE
    acc_t s = v >> shift;
    act_t r;  /* W8A4: clamp to symmetric 4-bit range -7..7 (see fastvit_ip.h) */
    if      (s >  7) r =  7;
    else if (s < -7) r = -7;
    else             r = (act_t)s;
    if (relu && r < 0) r = 0;
    return r;
}

/* Masked single-nibble read from a packed pack_t* port (W8A4: 8 nibble
 * lanes/word, was 4 byte lanes). Less bandwidth efficient than a batch
 * preload (each read fetches a whole word to use 1 nibble), but
 * conv_worker is only ever invoked once per inference (the stem layer)
 * and was never the bottleneck -- correctness and minimal risk to the
 * already-tuned tiling logic take priority here. */
static inline act_t read_byte(pack_t *base, int idx) {
#pragma HLS INLINE
    pack_t w = base[idx >> 3];
    int sel = idx & 0x7;
    return (act_t)w.range(sel*4+3, sel*4);
}

/* Masked read-modify-write of 4 consecutive nibble lanes (one CONV_TC=4
 * tile-row's worth of output) into their half of an 8-lane pack_t word.
 * Safe because CONV_TC=4 tiles always land entirely within one half of a
 * word (col_out_base is always a multiple of CONV_TC, and this project's
 * Wout values are always multiples of 8 -- see conv_worker.cpp W8A4
 * conversion notes), and LOOP_TC runs strictly sequentially so the two
 * tc iterations that share a word's two halves never race: whichever
 * runs second simply preserves the first's already-written half via the
 * read-back, in either order. */
static inline void write_nibble_group4(pack_t *base, int global_idx_start,
                                        act_t v0, act_t v1, act_t v2, act_t v3) {
#pragma HLS INLINE
    int word_idx = global_idx_start >> 3;
    int half     = (global_idx_start >> 2) & 0x1;
    pack_t word = base[word_idx];
    if (half == 0) {
        word.range( 3,  0) = v0;
        word.range( 7,  4) = v1;
        word.range(11,  8) = v2;
        word.range(15, 12) = v3;
    } else {
        word.range(19, 16) = v0;
        word.range(23, 20) = v1;
        word.range(27, 24) = v2;
        word.range(31, 28) = v3;
    }
    base[word_idx] = word;
}

void conv_worker(
    pack_t in_a[],
    wt_t   in_b[],
    acc_t  bias[],
    pack_t out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    CHout,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    act_mode,
    int    out_shift)
{
#pragma HLS ARRAY_PARTITION variable=in_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=wt_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=wt_buf  dim=2 complete
#pragma HLS ARRAY_PARTITION variable=out_buf dim=1 complete

    int Hout = (Hin + 2*pad_h - CONV_K) / stride_h + 1;
    int Wout = (Win + 2*pad_w - CONV_K) / stride_w + 1;

    int Tm_loops = (CHout + CONV_TM - 1) / CONV_TM;
    int Tn_loops = (CHin  + CONV_TN - 1) / CONV_TN;
    int Tr_loops = (Hout  + CONV_TR - 1) / CONV_TR;
    int Tc_loops = (Wout  + CONV_TC - 1) / CONV_TC;

    int relu = (act_mode == ACT_RELU) ? 1 : 0;

    LOOP_TM:
    for (int tm = 0; tm < Tm_loops; tm++) {
        int cout_base = tm * CONV_TM;
        int cout_end  = (cout_base + CONV_TM < CHout) ? cout_base + CONV_TM : CHout;
        int tm_valid  = cout_end - cout_base;

        LOOP_TR:
        for (int tr = 0; tr < Tr_loops; tr++) {
            int row_out_base = tr * CONV_TR;
            int row_out_end  = (row_out_base + CONV_TR < Hout) ? row_out_base + CONV_TR : Hout;
            int tr_valid     = row_out_end - row_out_base;
            int row_in_base  = row_out_base * stride_h - pad_h;
            int in_tile_h    = (tr_valid - 1) * stride_h + CONV_K;

            LOOP_TC:
            for (int tc = 0; tc < Tc_loops; tc++) {
                int col_out_base = tc * CONV_TC;
                int col_out_end  = (col_out_base + CONV_TC < Wout) ? col_out_base + CONV_TC : Wout;
                int tc_valid     = col_out_end - col_out_base;
                int col_in_base  = col_out_base * stride_w - pad_w;
                int in_tile_w    = (tc_valid - 1) * stride_w + CONV_K;

                INIT_OUT:
                for (int m = 0; m < CONV_TM; m++) {
                    int ch_out = (m < tm_valid) ? (cout_base + m) : cout_base;
                    for (int r = 0; r < CONV_TR; r++) {
#pragma HLS PIPELINE II=1
                        for (int c = 0; c < CONV_TC; c++) {
                            out_buf[m][r][c] = bias[ch_out];
                        }
                    }
                }

                LOOP_TN:
                for (int tn = 0; tn < Tn_loops; tn++) {
                    int cin_base = tn * CONV_TN;
                    int cin_end  = (cin_base + CONV_TN < CHin) ? cin_base + CONV_TN : CHin;
                    int tn_valid = cin_end - cin_base;

                    LOAD_IN:
                    for (int n = 0; n < CONV_TN; n++) {
                        int ch = cin_base + n;
                        int ch_base_addr = ch * Hin * Win;
                        for (int r = 0; r < CONV_IN_TILE_H; r++) {
                            int in_r = row_in_base + r;
                            int row_base = ch_base_addr + in_r * Win;
                            bool row_valid = (n < tn_valid) && (in_r >= 0) && (in_r < Hin) && (r < in_tile_h);
#pragma HLS PIPELINE II=1
                            for (int c = 0; c < CONV_IN_TILE_W; c++) {
                                int in_c = col_in_base + c;
                                if (row_valid && in_c >= 0 && in_c < Win && c < in_tile_w)
                                    in_buf[n][r][c] = read_byte(in_a, row_base + in_c);
                                else
                                    in_buf[n][r][c] = 0;
                            }
                        }
                    }

                    LOAD_WT:
                    for (int m = 0; m < CONV_TM; m++) {
                        for (int n = 0; n < CONV_TN; n++) {
                            for (int kh = 0; kh < CONV_K; kh++) {
#pragma HLS PIPELINE II=1
                                for (int kw = 0; kw < CONV_K; kw++) {
                                    int cout_idx = cout_base + m;
                                    int cin_idx  = cin_base  + n;
                                    if (m < tm_valid && n < tn_valid)
                                        wt_buf[m][n][kh][kw] = in_b[
                                            cout_idx * CHin * CONV_K * CONV_K +
                                            cin_idx  * CONV_K * CONV_K +
                                            kh * CONV_K + kw];
                                    else
                                        wt_buf[m][n][kh][kw] = 0;
                                }
                            }
                        }
                    }

                    COMPUTE_KH:
                    for (int kh = 0; kh < CONV_K; kh++) {
                        COMPUTE_KW:
                        for (int kw = 0; kw < CONV_K; kw++) {
                            COMPUTE_TR:
                            for (int r = 0; r < CONV_TR; r++) {
                                int ir = (stride_h == 1) ? (r + kh) : (r + r + kh);
                                COMPUTE_TC:
                                for (int c = 0; c < CONV_TC; c++) {
#pragma HLS PIPELINE II=1
                                    int ic = (stride_w == 1) ? (c + kw) : (c + c + kw);
                                    COMPUTE_TM:
                                    for (int m = 0; m < CONV_TM; m++) {
#pragma HLS UNROLL
                                        acc_t sum = 0;
                                        COMPUTE_TN:
                                        for (int n = 0; n < CONV_TN; n++) {
#pragma HLS UNROLL
                                            sum += (acc_t)in_buf[n][ir][ic] * (acc_t)wt_buf[m][n][kh][kw];
                                        }
                                        out_buf[m][r][c] += sum;
                                    }
                                }
                            }
                        }
                    }
                } // tn

                /* WRITE_OUT (W8A4): CONV_TC=4 no longer fills a whole pack_t
                 * word by itself (8 nibble lanes/word now, not 4 byte lanes),
                 * so each tile-row's 4 values only cover HALF a word --
                 * write via a masked read-modify-write instead of a single
                 * clean word store. See write_nibble_group4()'s comment for
                 * why this is safe (col_out_base/Wout alignment + strictly
                 * sequential tc iterations). */
                WRITE_OUT:
                for (int m = 0; m < CONV_TM; m++) {
                    if (m >= tm_valid) break;
                    int ch_out = cout_base + m;
                    int ch_base_out = ch_out * Hout * Wout;
                    for (int r = 0; r < CONV_TR; r++) {
                        if (r >= tr_valid) break;
                        int row_base_out = ch_base_out + (row_out_base + r) * Wout;
#pragma HLS PIPELINE II=1
                        act_t v0 = (0 < tc_valid) ? conv_quant_act(out_buf[m][r][0], out_shift, relu) : (act_t)0;
                        act_t v1 = (1 < tc_valid) ? conv_quant_act(out_buf[m][r][1], out_shift, relu) : (act_t)0;
                        act_t v2 = (2 < tc_valid) ? conv_quant_act(out_buf[m][r][2], out_shift, relu) : (act_t)0;
                        act_t v3 = (3 < tc_valid) ? conv_quant_act(out_buf[m][r][3], out_shift, relu) : (act_t)0;
                        write_nibble_group4(out, row_base_out + col_out_base, v0, v1, v2, v3);
                    }
                }

            } // tc
        } // tr
    } // tm
}
