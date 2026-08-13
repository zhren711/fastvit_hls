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
    act_t r;
    if      (s >  127) r =  127;
    else if (s < -128) r = -128;
    else               r = (act_t)s;
    if (relu && r < 0) r = 0;
    return r;
}

/* Masked single-byte read from a packed pack_t* port. Less bandwidth
 * efficient than a batch preload (each read fetches a whole word to use
 * 1 byte), but conv_worker is only ever invoked once per inference (the
 * stem layer) and was never the bottleneck -- correctness and minimal
 * risk to the already-tuned tiling logic take priority here. */
static inline act_t read_byte(pack_t *base, int idx) {
#pragma HLS INLINE
    pack_t w = base[idx >> 2];
    int sel = idx & 0x3;
    return (act_t)w.range(sel*8+7, sel*8);
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

                /* WRITE_OUT: packed 4-byte-per-word write. CONV_TC==4 matches
                 * the pack width exactly, and col_out_base=tc*CONV_TC / row
                 * starts are multiples of CONV_TC given Wout%4==0, so every
                 * (m,r) tile row of CONV_TC output columns lands on exactly
                 * one aligned pack_t word. */
                WRITE_OUT:
                for (int m = 0; m < CONV_TM; m++) {
                    if (m >= tm_valid) break;
                    int ch_out = cout_base + m;
                    int ch_base_out = ch_out * Hout * Wout;
                    for (int r = 0; r < CONV_TR; r++) {
                        if (r >= tr_valid) break;
                        int row_base_out = ch_base_out + (row_out_base + r) * Wout;
#pragma HLS PIPELINE II=1
                        pack_t word;
                        for (int c = 0; c < CONV_TC; c++) {
#pragma HLS UNROLL
                            act_t v = (c < tc_valid) ? conv_quant_act(out_buf[m][r][c], out_shift, relu) : (act_t)0;
                            word.range(c*8+7, c*8) = v;
                        }
                        int word_idx = (row_base_out + col_out_base) >> 2;
                        out[word_idx] = word;
                    }
                }

            } // tc
        } // tr
    } // tm
}
