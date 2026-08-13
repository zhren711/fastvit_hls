/*============================================================
 * pwconv_worker.cpp — ported from pwconv_ip/pwconv_ip.cpp v14, with
 * ONE correctness fix on top of the port (INTERFACE pragma removal +
 * port renames): the original's TM>TN>TS loop nesting reused the
 * pw_out_buf accumulator (sized for only ONE spatial tile) across
 * every TS iteration inside each TN pass, discarding partial sums
 * before all input channels (TN) had been accumulated for a given
 * spatial tile, whenever Tn_loops>1 and Ts_loops>1 (i.e. essentially
 * always for real layers -- CHin>4, spatial>8). Verified by hand
 * trace and confirmed empirically: 100% of pwconv csim tests failed
 * before this fix (dwconv/add, unaffected by this bug, passed).
 * Fix: reorder to TM>TS>TN (TN now innermost) so pw_out_buf is
 * initialized once per (tm,ts), accumulates across ALL tn for that
 * ts, and is written out only after the tn loop completes. Cost:
 * weights are now reloaded per (tm,ts,tn) instead of per (tm,tn) --
 * more AXI traffic for weight reads, but the compute result is now
 * correct. No csim log for the original pwconv_ip was ever found in
 * pwconv_ip_proj, suggesting this was never caught by simulation.
 *
 * (2026-07-30: a whole-tensor input cache was tried here to cut
 * redundant per-tm DRAM re-reads on high-Tm_loops layers, and it did
 * pass csim, but reverted after board measurement showed it was a net
 * REGRESSION: the cache stored the input unpacked (1 byte/BRAM word),
 * so its LOAD_PW_IN read path cost 4x more cycles per (tm,tn) tile
 * than the packed 4-bytes/cycle DRAM burst path it replaced -- Stage4
 * PW1/PW2 got 33% SLOWER (82.68ms->109.75ms) because Tm_loops*Tn_loops
 * there is large enough that the extra per-tile overhead swamped the
 * DRAM-read savings. csim only checks correctness, not cycle cost, so
 * it couldn't catch this. Back to the plain per-(tm,ts,tn) DRAM read
 * below; see fastvit_ip.h history for the removed PW_CACHE_BUDGET_BYTES.)
 *============================================================*/

#include "pwconv_worker.h"

static act_t pw_in_buf [PW_TN][PW_TS];
static wt_t  pw_wt_buf [PW_TM][PW_TN];
static acc_t pw_out_buf[PW_TM][PW_MAX_SPATIAL];

static inline act_t pwconv_apply_act(acc_t val, int act_mode, int shift) {
#pragma HLS INLINE
    acc_t s = val >> shift;
    act_t r;  /* W8A4: clamp to symmetric 4-bit range -7..7 (see fastvit_ip.h) */
    if      (s >  7) r =  7;
    else if (s < -7) r = -7;
    else             r = (act_t)s;
    if (act_mode == ACT_RELU && r < 0) r = 0;
    return r;
}

void pwconv_worker(
    pack_t feat_in_w32[],
    wt_t   weight[],
    acc_t  bias[],
    pack_t feat_out_w32[],
    int    CHin,
    int    H,
    int    W,
    int    CHout,
    int    act_mode,
    int    out_shift)
{
#pragma HLS ARRAY_PARTITION variable=pw_in_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=pw_in_buf  dim=2 complete
#pragma HLS ARRAY_PARTITION variable=pw_wt_buf  dim=1 complete
#pragma HLS ARRAY_PARTITION variable=pw_wt_buf  dim=2 complete
/* pw_out_buf: dim=1 (TM=8) complete -> 8 independent BRAM banks so all 8
 * output channels can be read/accumulated/written in parallel each cycle;
 * dim=2 (spatial, up to PW_MAX_SPATIAL) stays BRAM-addressed, not
 * partitioned -- that's the whole point, it's what lets weights stay
 * resident across the entire spatial range instead of being reloaded
 * per spatial tile. */
#pragma HLS ARRAY_PARTITION variable=pw_out_buf dim=1 complete
#pragma HLS BIND_STORAGE variable=pw_out_buf type=RAM_1P impl=BRAM

    int spatial  = H * W;
    int Tm_loops = (CHout   + PW_TM - 1) / PW_TM;
    int Tn_loops = (CHin    + PW_TN - 1) / PW_TN;
    int Ts_loops = (spatial + PW_TS - 1) / PW_TS;

    LOOP_PW_TM:
    for (int tm = 0; tm < Tm_loops; tm++) {
        int cout_base = tm * PW_TM;
        int cout_end  = (cout_base + PW_TM < CHout) ? (cout_base + PW_TM) : CHout;
        int tm_valid  = cout_end - cout_base;

        /* Init once per tm, covering the FULL spatial range (not just one
         * tile) -- this is what lets the tn loop below accumulate into it
         * across the entire feature map before ever writing out. */
        INIT_PW_OUT:
        for (int m = 0; m < PW_TM; m++) {
#pragma HLS UNROLL
            acc_t b = (m < tm_valid) ? bias[cout_base + m] : (acc_t)0;
            for (int s = 0; s < spatial; s++) {
#pragma HLS PIPELINE II=1
                pw_out_buf[m][s] = b;
            }
        }

        LOOP_PW_TN:
        for (int tn = 0; tn < Tn_loops; tn++) {
            int cin_base = tn * PW_TN;
            int cin_end  = (cin_base + PW_TN < CHin) ? (cin_base + PW_TN) : CHin;
            int tn_valid = cin_end - cin_base;

            /* Weight tile loaded ONCE per (tm,tn) -- reused across the
             * entire spatial range via the ts loop below, instead of
             * being reloaded per (tm,ts,tn) (the ~25x-slower bug). */
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

                /* W8A4: 8 nibble-packed act_t lanes per 32-bit pack_t word
                 * (was 4 byte lanes) -- see fastvit_ip.h. PW_TS=64 is a
                 * multiple of 8, so PW_TS/8 divides evenly, same as the
                 * PW_TS/4 it replaces. */
                LOAD_PW_IN:
                for (int n = 0; n < PW_TN; n++) {
                    for (int sw = 0; sw < PW_TS/8; sw++) {
#pragma HLS PIPELINE II=1
                        unsigned ch_base_word = (unsigned)(cin_base + n) * (unsigned)spatial;
                        unsigned row_addr = (ch_base_word + (unsigned)sp_base) >> 3;
                        if (n < tn_valid) {
                            pack_t word = feat_in_w32[row_addr + sw];
                            pw_in_buf[n][sw*8+0] = (act_t)word.range( 3,  0);
                            pw_in_buf[n][sw*8+1] = (act_t)word.range( 7,  4);
                            pw_in_buf[n][sw*8+2] = (act_t)word.range(11,  8);
                            pw_in_buf[n][sw*8+3] = (act_t)word.range(15, 12);
                            pw_in_buf[n][sw*8+4] = (act_t)word.range(19, 16);
                            pw_in_buf[n][sw*8+5] = (act_t)word.range(23, 20);
                            pw_in_buf[n][sw*8+6] = (act_t)word.range(27, 24);
                            pw_in_buf[n][sw*8+7] = (act_t)word.range(31, 28);
                        } else {
                            pw_in_buf[n][sw*8+0] = 0;
                            pw_in_buf[n][sw*8+1] = 0;
                            pw_in_buf[n][sw*8+2] = 0;
                            pw_in_buf[n][sw*8+3] = 0;
                            pw_in_buf[n][sw*8+4] = 0;
                            pw_in_buf[n][sw*8+5] = 0;
                            pw_in_buf[n][sw*8+6] = 0;
                            pw_in_buf[n][sw*8+7] = 0;
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
                        pw_out_buf[m][sp_base + s] += dot;
                    }
                }
            } // ts
        } // tn -- all input channels now accumulated across the WHOLE layer for this tm

        /* Write out once per tm, after all tn (and hence all input
         * channels) are fully accumulated -- covers the full spatial
         * range, not just one tile. */
        {
            /* W8A4: 8 nibble lanes/word (was 4 byte lanes) -- see
             * fastvit_ip.h. spatial is always a power of 2 >= 8 for this
             * network (64x64 down to 4x4), so spatial/8 always divides
             * evenly, same as the spatial/4 it replaces. */
            unsigned word_spatial_w = (unsigned)spatial >> 3;
            unsigned word_base_out = (unsigned)cout_base * word_spatial_w;
            unsigned sw_words = (unsigned)spatial >> 3;
            WRITE_PW_OUT:
            for (int m = 0; m < tm_valid; m++) {
                for (unsigned sw = 0; sw < sw_words; sw++) {
#pragma HLS PIPELINE II=1
                    pack_t word;
                    word.range( 3,  0) = pwconv_apply_act(pw_out_buf[m][sw*8+0], act_mode, out_shift);
                    word.range( 7,  4) = pwconv_apply_act(pw_out_buf[m][sw*8+1], act_mode, out_shift);
                    word.range(11,  8) = pwconv_apply_act(pw_out_buf[m][sw*8+2], act_mode, out_shift);
                    word.range(15, 12) = pwconv_apply_act(pw_out_buf[m][sw*8+3], act_mode, out_shift);
                    word.range(19, 16) = pwconv_apply_act(pw_out_buf[m][sw*8+4], act_mode, out_shift);
                    word.range(23, 20) = pwconv_apply_act(pw_out_buf[m][sw*8+5], act_mode, out_shift);
                    word.range(27, 24) = pwconv_apply_act(pw_out_buf[m][sw*8+6], act_mode, out_shift);
                    word.range(31, 28) = pwconv_apply_act(pw_out_buf[m][sw*8+7], act_mode, out_shift);
                    feat_out_w32[word_base_out + sw] = word;
                }
                word_base_out += word_spatial_w;
            }
        }

    } // tm
}
