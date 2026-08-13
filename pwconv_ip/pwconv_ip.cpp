/*============================================================
 * pwconv_ip.cpp  v14 (final for xc7z020)
 * (Tm,Tn,Ts) 循环 + 32-bit AXI + NCHW
 * TM=8, TN=4, TS=8
 *============================================================*/

#include "pwconv_ip.h"

static act_t pw_in_buf [PW_TN][PW_TS];
static wt_t  pw_wt_buf [PW_TM][PW_TN];
static acc_t pw_out_buf[PW_TM][PW_TS];

static inline act_t apply_act(acc_t val, int act_mode, int shift) {
#pragma HLS INLINE
    acc_t s = val >> shift;
    act_t r;
    if      (s >  127) r =  127;
    else if (s < -128) r = -128;
    else               r = (act_t)s;
    if (act_mode == ACT_RELU && r < 0) r = 0;
    return r;
}

void pwconv_ip(
    data32_t feat_in_w32[],
    wt_t     weight[],
    acc_t    bias[],
    data32_t feat_out_w32[],
    int      CHin,
    int      H,
    int      W,
    int      CHout,
    int      act_mode,
    int      out_shift)
{
#pragma HLS INTERFACE m_axi port=feat_in_w32  offset=slave bundle=gmem0 depth=131072 \
    max_read_burst_length=16 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=weight       offset=slave bundle=gmem1 depth=262144 \
    max_read_burst_length=16 num_read_outstanding=16
#pragma HLS INTERFACE m_axi port=bias         offset=slave bundle=gmem2 depth=1152
#pragma HLS INTERFACE m_axi port=feat_out_w32 offset=slave bundle=gmem3 depth=131072 \
    max_write_burst_length=16 num_write_outstanding=16

#pragma HLS INTERFACE s_axilite port=CHin      bundle=ctrl
#pragma HLS INTERFACE s_axilite port=H         bundle=ctrl
#pragma HLS INTERFACE s_axilite port=W         bundle=ctrl
#pragma HLS INTERFACE s_axilite port=CHout     bundle=ctrl
#pragma HLS INTERFACE s_axilite port=act_mode  bundle=ctrl
#pragma HLS INTERFACE s_axilite port=out_shift bundle=ctrl
#pragma HLS INTERFACE s_axilite port=return    bundle=ctrl

#pragma HLS ARRAY_PARTITION variable=pw_in_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=pw_in_buf  dim=2 complete
#pragma HLS ARRAY_PARTITION variable=pw_wt_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=pw_wt_buf  dim=2 complete
#pragma HLS ARRAY_PARTITION variable=pw_out_buf dim=1 complete
#pragma HLS ARRAY_PARTITION variable=pw_out_buf dim=2 complete

    int spatial  = H * W;
    int Tm_loops = (CHout   + PW_TM - 1) / PW_TM;
    int Tn_loops = (CHin    + PW_TN - 1) / PW_TN;
    int Ts_loops = (spatial + PW_TS - 1) / PW_TS;

    unsigned chin_words = (unsigned)CHin >> 2;

    LOOP_PW_TM:
    for (int tm = 0; tm < Tm_loops; tm++) {
        int cout_base = tm * PW_TM;
        int cout_end  = (cout_base + PW_TM < CHout) ? (cout_base + PW_TM) : CHout;
        int tm_valid  = cout_end - cout_base;

        LOOP_PW_TN:
        for (int tn = 0; tn < Tn_loops; tn++) {
            int cin_base = tn * PW_TN;
            int cin_end  = (cin_base + PW_TN < CHin) ? (cin_base + PW_TN) : CHin;
            int tn_valid = cin_end - cin_base;

            LOAD_PW_WT:
            for (int m = 0; m < PW_TM; m++) {
                int row_base = (cout_base + m) * CHin + cin_base;
#pragma HLS PIPELINE II=1
                for (int n = 0; n < PW_TN; n++) {
                    pw_wt_buf[m][n] = (m < tm_valid && n < tn_valid) ?
                        weight[row_base + n] : (wt_t)0;
                }
            }

            LOOP_PW_TS:
            for (int ts = 0; ts < Ts_loops; ts++) {
                int sp_base  = ts * PW_TS;
                int sp_end   = (sp_base + PW_TS < spatial) ? (sp_base + PW_TS) : spatial;
                int sp_valid = sp_end - sp_base;

                if (tn == 0) {
                    INIT_PW_OUT:
                    for (int m = 0; m < PW_TM; m++) {
#pragma HLS UNROLL
                        acc_t b = (m < tm_valid) ? bias[cout_base + m] : (acc_t)0;
                        for (int s = 0; s < PW_TS; s++) {
#pragma HLS UNROLL
                            pw_out_buf[m][s] = b;
                        }
                    }
                }

                /* LOAD_PW_IN: NCHW, 32-bit AXI
                 * 每 (Tm,Tn,Ts) tile: TN channels × (TS/4=2) 32-bit reads = 8 reads
                 * trip count = PW_TS/1 = 8 (per channel, fixed) */
                {
                    unsigned cin_word = (unsigned)cin_base >> 2;
                    LOAD_PW_IN:
                    for (int n = 0; n < PW_TN; n++) {
                        unsigned ch_base_word = (unsigned)(cin_base + n) * (unsigned)spatial;
                        for (int sw = 0; sw < PW_TS/4; sw++) {
#pragma HLS PIPELINE II=1
                            unsigned base_addr = (ch_base_word + (unsigned)sp_base) >> 2;
                            if (n < tn_valid) {
                                data32_t word = feat_in_w32[base_addr + sw];
                                pw_in_buf[n][sw*4+0] = (act_t)word.range(7,  0);
                                pw_in_buf[n][sw*4+1] = (act_t)word.range(15, 8);
                                pw_in_buf[n][sw*4+2] = (act_t)word.range(23, 16);
                                pw_in_buf[n][sw*4+3] = (act_t)word.range(31, 24);
                            } else {
                                pw_in_buf[n][sw*4+0] = 0;
                                pw_in_buf[n][sw*4+1] = 0;
                                pw_in_buf[n][sw*4+2] = 0;
                                pw_in_buf[n][sw*4+3] = 0;
                            }
                        }
                    }
                }

                COMPUTE_PW_S:
                for (int s = 0; s < sp_valid; s++) {
#pragma HLS PIPELINE II=1
                    COMPUTE_PW_TM:
                    for (int m = 0; m < PW_TM; m++) {
#pragma HLS UNROLL
                        acc_t dot = 0;
                        COMPUTE_PW_TN:
                        for (int n = 0; n < PW_TN; n++) {
#pragma HLS UNROLL
                            dot += (acc_t)pw_in_buf[n][s] * (acc_t)pw_wt_buf[m][n];
                        }
                        pw_out_buf[m][s] += dot;
                    }
                }

                if (tn == Tn_loops - 1) {
                    unsigned word_spatial_w = (unsigned)spatial >> 2;
                    unsigned word_base_out =
                        ((unsigned)cout_base * (unsigned)spatial + (unsigned)sp_base) >> 2;
                    WRITE_PW_OUT:
                    for (int m = 0; m < tm_valid; m++) {
                        for (int sw = 0; sw < PW_TS/4; sw++) {
#pragma HLS PIPELINE II=1
                            data32_t word;
                            word.range(7,  0)  = apply_act(pw_out_buf[m][sw*4+0], act_mode, out_shift);
                            word.range(15, 8)  = apply_act(pw_out_buf[m][sw*4+1], act_mode, out_shift);
                            word.range(23, 16) = apply_act(pw_out_buf[m][sw*4+2], act_mode, out_shift);
                            word.range(31, 24) = apply_act(pw_out_buf[m][sw*4+3], act_mode, out_shift);
                            feat_out_w32[word_base_out + sw] = word;
                        }
                        word_base_out += word_spatial_w;
                    }
                }

            } // ts
        } // tn
    } // tm
}
