/*============================================================
 * dwconv_worker.cpp — compute core ported from dwconv_ip/dwconv_ip.cpp
 * v14 (TR-sequential, TC-unroll). v13's TR+TC double-unroll produced
 * 1,995 control sets and failed Vivado placement on the OLD 5-separate-IP
 * top level; the failure turned out to be ~77% interconnect/per-IP AXI
 * adapter plumbing (see report_control_sets analysis, 2026-07-28), not
 * dwconv's own compute logic -- so it's being retried here inside the
 * unified fastvit_ip (4 shared m_axi masters, 1 SmartConnect tier)
 * instead of the old top level. Interface pragmas removed (now live on
 * the fastvit_ip top function), ports renamed to the shared names.
 *
 * 2026-08-09: reverted to this single-monolithic-function form (matching
 * dwconv_ip.cpp's original structure exactly, just with W8A4 nibble
 * packing) -- restores the state that produced this project's best-ever
 * Tier A 200MHz result, WNS=-2.164ns (2026-08-01, multicycle constraint
 * + AggressiveExplore phys_opt). A later same-day change split PRELOAD_IN
 * into a separate DATAFLOW-fed function (dwconv_preload_channel) to
 * decouple it from the rest of the per-channel work; that split was
 * motivated by -2.164ns's own newly-exposed bottleneck (gmem0's ARREADY-
 * derived FSM signal reaching into gmem1's FIFO logic) but was never
 * itself confirmed to fix it in Tier A -- the user moved to Tier B
 * instead right after -2.164ns was reported. This session's real,
 * measured Tier A rebuild with the split still in place regressed to
 * -3.10ns (worse than the -2.56ns "bare" baseline the split was never
 * actually part of), so it's reverted here along with the 2026-08-06/07
 * LOAD_DW_IN/ch_out_buf/CHin_n 200MHz-timing-driven changes (see git-less
 * history in project memory, 2026-08-09 session) to reproduce -2.164ns's
 * exact source configuration for real, not from memory.
 *============================================================*/

#include "dwconv_worker.h"

static act_t ch_in_buf [DW_MAX_H * DW_MAX_W];
static act_t ch_out_buf[DW_MAX_H * DW_MAX_W];

static act_t dw_in_buf [DW_TN][DW_MAX_IN_TILE_H][DW_MAX_IN_TILE_W];
static wt_t  dw_wt_buf [DW_TN][DW_MAX_K][DW_MAX_K];
static acc_t dw_out_buf[DW_TN][DW_TR][DW_TC];

static inline act_t dwconv_apply_act(acc_t val, int act_mode, int shift) {
#pragma HLS INLINE
    acc_t s = val >> shift;
    act_t r;  /* W8A4: clamp to symmetric 4-bit range -7..7 (see fastvit_ip.h) */
    if      (s >  7) r =  7;
    else if (s < -7) r = -7;
    else             r = (act_t)s;
    if (act_mode == ACT_RELU && r < 0) r = 0;
    return r;
}

/* DSP-packing (W8A4) TRIED AND REVERTED, 2026-08-01: prototyped packing
 * 2 independent int4 activation x int8 weight multiplies into ONE
 * DSP48E1's native 25x18 signed multiplier (12-bit slots, adapted from
 * the abandoned int8xint8 version dwconv_ip/dsp_pack_design.cpp, v13,
 * FILM-QNN Fig.1(b) style, which used 16-bit slots and was abandoned
 * for the same reason found here). csim PASSED (functionally correct),
 * but the real csynth numbers were decisive: DSP 125->123 (-2, because
 * dwconv was never the dominant DSP consumer -- pwconv's TM*TN=8*8=64
 * DSPs dwarf dwconv's DW_TC=4) at a cost of LUT 68,416->68,584 (+168,
 * the packing/unpacking glue -- pre-adder shift, sign-fixup compare --
 * costs more LUT than 2 DSPs are worth). This is the SAME failure mode
 * v13 hit at 8-bit, now confirmed to also hold at 4-bit: the DSP48E1's
 * fixed 25x18 width caps packing at 2 slots regardless of activation
 * width (as long as the weight stays 8-bit), so narrowing activations
 * doesn't unlock MORE packing density, only a marginally cheaper
 * packing structure for the same 2 slots -- not cheap enough to beat
 * the glue overhead on this LUT-bound (not DSP-bound) xc7z020, where
 * DSPs were never the scarce resource to begin with. Do not retry DSP
 * packing on this chip without a fundamentally different technique
 * (this closes off both the 8-bit and 4-bit versions of the
 * "pack 2 activations into 1 DSP" idea specifically). */

void dwconv_worker(
    pack_t feat_in[],
    wt_t   weight[],
    acc_t  bias[],
    pack_t feat_out[],
    int    CHin,
    int    Hin,
    int    Win,
    int    Kh,
    int    Kw,
    int    stride_h,
    int    stride_w,
    int    pad_h,
    int    pad_w,
    int    fpg,
    int    act_mode,
    int    out_shift)
{
/* ch_in_buf: cyclic=16 -> 16 BRAM banks
 * LOAD_DW_IN reads 13 consecutive cols -> 13 different banks -> II=1 achievable */
#pragma HLS ARRAY_PARTITION variable=ch_in_buf  cyclic factor=16 dim=1
#pragma HLS ARRAY_PARTITION variable=ch_out_buf cyclic factor=16 dim=1
#pragma HLS BIND_STORAGE variable=ch_in_buf  type=RAM_1P impl=BRAM
#pragma HLS BIND_STORAGE variable=ch_out_buf type=RAM_1P impl=BRAM

/* dw_in_buf/wt_buf: fully partitioned -> all elements as registers
 * row_buf/wt_row: 1D staging buffers, pre-loaded per (kh,r) to cut MUX depth */
#pragma HLS ARRAY_PARTITION variable=dw_in_buf  complete dim=0
#pragma HLS ARRAY_PARTITION variable=dw_wt_buf  complete dim=0
#pragma HLS ARRAY_PARTITION variable=dw_out_buf complete dim=0

    act_t row_buf[DW_MAX_IN_TILE_W];
#pragma HLS ARRAY_PARTITION variable=row_buf complete dim=1
    wt_t  wt_row[DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=wt_row complete dim=1

    int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    int Tr_loops = (Hout + DW_TR - 1) / DW_TR;
    int Tc_loops = (Wout + DW_TC - 1) / DW_TC;
    /* W8A4: 8 nibble-packed act_t lanes per 32-bit pack_t word (was 4
     * byte lanes when act_t was 8-bit) -- see fastvit_ip.h. */
    int hw_in_words  = (Hin * Win)   >> 3;
    int hw_out_words = (Hout * Wout) >> 3;

    LOOP_DW_CH:
    for (int ch = 0; ch < CHin; ch++) {
#pragma HLS LOOP_TRIPCOUNT min=48 max=512

        PRELOAD_IN:
        for (int hw = 0; hw < hw_in_words; hw++) {
#pragma HLS PIPELINE II=1
            ap_uint<32> word = feat_in[ch * hw_in_words + hw];
            ch_in_buf[hw*8+0] = (act_t)word.range( 3,  0);
            ch_in_buf[hw*8+1] = (act_t)word.range( 7,  4);
            ch_in_buf[hw*8+2] = (act_t)word.range(11,  8);
            ch_in_buf[hw*8+3] = (act_t)word.range(15, 12);
            ch_in_buf[hw*8+4] = (act_t)word.range(19, 16);
            ch_in_buf[hw*8+5] = (act_t)word.range(23, 20);
            ch_in_buf[hw*8+6] = (act_t)word.range(27, 24);
            ch_in_buf[hw*8+7] = (act_t)word.range(31, 28);
        }

        LOOP_DW_FPG:
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;

            LOAD_DW_WT:
            for (int kh = 0; kh < Kh; kh++) {
#pragma HLS PIPELINE II=1
                for (int kw = 0; kw < Kw; kw++) {
                    dw_wt_buf[0][kh][kw] = weight[co * Kh * Kw + kh * Kw + kw];
                }
            }

            acc_t bias_val = bias[co];

            LOOP_DW_TR:
            for (int tr = 0; tr < Tr_loops; tr++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=16
                int row_out_base = tr * DW_TR;
                int row_out_end  = (row_out_base + DW_TR < Hout) ? (row_out_base + DW_TR) : Hout;
                int tr_valid     = row_out_end - row_out_base;
                int row_in_base  = row_out_base * stride_h - pad_h;
                int in_tile_h    = (tr_valid - 1) * stride_h + Kh;

                LOOP_DW_TC:
                for (int tc = 0; tc < Tc_loops; tc++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=16
                    int col_out_base = tc * DW_TC;
                    int col_out_end  = (col_out_base + DW_TC < Wout) ? (col_out_base + DW_TC) : Wout;
                    int tc_valid     = col_out_end - col_out_base;
                    int col_in_base  = col_out_base * stride_w - pad_w;
                    int in_tile_w    = (tc_valid - 1) * stride_w + Kw;

                    LOAD_DW_IN:
                    for (int r = 0; r < DW_MAX_IN_TILE_H; r++) {
#pragma HLS PIPELINE II=1
                        int in_r = row_in_base + r;
                        if (r < in_tile_h) {
                            for (int c = 0; c < DW_MAX_IN_TILE_W; c++) {
#pragma HLS UNROLL
                                int in_c = col_in_base + c;
                                dw_in_buf[0][r][c] =
                                    (in_r >= 0 && in_r < Hin &&
                                     c < in_tile_w &&
                                     in_c >= 0 && in_c < Win)
                                    ? ch_in_buf[in_r * Win + in_c]
                                    : (act_t)0;
                            }
                        }
                    }

                    CLEAR_DW_OUT:
                    for (int r = 0; r < DW_TR; r++) {
#pragma HLS UNROLL
                        for (int c = 0; c < DW_TC; c++) {
#pragma HLS UNROLL
                            dw_out_buf[0][r][c] = bias_val;
                        }
                    }

                    COMPUTE_DW_TR:
                    for (int r = 0; r < DW_TR; r++) {
                        COMPUTE_DW_KH:
                        for (int kh = 0; kh < Kh; kh++) {
                            int ir = r * stride_h + kh;
                            /* Pre-load row ir and weight row kh (1-cycle parallel load) */
                            for (int c = 0; c < DW_MAX_IN_TILE_W; c++) {
#pragma HLS UNROLL
                                row_buf[c] = dw_in_buf[0][ir][c];
                            }
                            for (int kw = 0; kw < DW_MAX_K; kw++) {
#pragma HLS UNROLL
                                wt_row[kw] = dw_wt_buf[0][kh][kw];
                            }
                            COMPUTE_DW_KW:
                            for (int kw = 0; kw < Kw; kw++) {
#pragma HLS PIPELINE II=1
                                wt_t w = wt_row[kw];
                                COMPUTE_DW_TC:
                                for (int c = 0; c < DW_TC; c++) {
#pragma HLS UNROLL
                                    int ic = c * stride_w + kw;
                                    dw_out_buf[0][r][c] += (acc_t)row_buf[ic] * (acc_t)w;
                                }
                            }
                        }
                    }

                    STORE_DW_OUT:
                    for (int r = 0; r < DW_TR; r++) {
#pragma HLS PIPELINE II=1
                        if (r < tr_valid) {
                            int oh = row_out_base + r;
                            for (int c = 0; c < DW_TC; c++) {
#pragma HLS UNROLL
                                if (c < tc_valid) {
                                    int ow = col_out_base + c;
                                    ch_out_buf[oh * Wout + ow] =
                                        dwconv_apply_act(dw_out_buf[0][r][c], act_mode, out_shift);
                                }
                            }
                        }
                    }

                } // tc
            } // tr

            WRITEBACK_OUT:
            for (int hw = 0; hw < hw_out_words; hw++) {
#pragma HLS PIPELINE II=1
                ap_uint<32> word;
                word.range( 3,  0) = ch_out_buf[hw*8+0];
                word.range( 7,  4) = ch_out_buf[hw*8+1];
                word.range(11,  8) = ch_out_buf[hw*8+2];
                word.range(15, 12) = ch_out_buf[hw*8+3];
                word.range(19, 16) = ch_out_buf[hw*8+4];
                word.range(23, 20) = ch_out_buf[hw*8+5];
                word.range(27, 24) = ch_out_buf[hw*8+6];
                word.range(31, 28) = ch_out_buf[hw*8+7];
                feat_out[co * hw_out_words + hw] = word;
            }

        } // fpg
    } // ch
}
