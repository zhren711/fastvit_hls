/*============================================================
 * dwconv_worker.cpp -- line-buffer / shift-register streaming rewrite
 *
 * Isolated feasibility study for the 200MHz dwconv architecture that was
 * scoped-but-deferred on 2026-07-28/08-07 (see project memory and plan
 * jaunty-questing-journal.md). The tile-based design (backed up as
 * dwconv_worker.tile_backup_2530ns.cpp/.h, restore from there if this
 * integration needs reverting) plateaued at isolated WNS=-0.196ns after
 * several rounds of targeted HLS fixes; its final remaining critical path
 * was a runtime-computed index (`ic = c*stride_w+kw`) reading a
 * fully-partitioned array (`row_buf[ic]`) -- a ~13:1 dynamic-index mux.
 * This rewrite eliminates every dynamic-index read/write in the hot
 * per-pixel path by replacing the "DMA a whole tile into BRAM, then
 * gather a window with computed offsets" scheme with the classical
 * streaming line-buffer/shift-window architecture: a small set of
 * Win-wide row buffers plus a KxK register window, addressed only by
 * fixed (compile-time-unrolled) indices and the natural raster-scan
 * column counter.
 *
 * 2026-08-11: swapped into the merged fastvit_ip (Tier A, 4 shared m_axi
 * masters) in place of the tile design, to get a real combined-design
 * number -- this project's repeated experience is that isolated-worker
 * timing wins have NOT reliably transferred to the combined/shared-
 * adapter design (see project memory for the prior instances), so this
 * is a genuine test, not an assumed win. Only the interface pragmas were
 * removed relative to the isolated version (they now live on fastvit_ip's
 * own top-level function, matching every other worker in this merge);
 * every design/compute-path decision below is unchanged from the
 * isolated study.
 *
 * DESIGN NOTES (read before touching indices -- every offset below is
 * deliberate, not arbitrary):
 *
 * 1. "Age" indexing, not "row/col position" indexing. window[i][j] holds
 *    the sample that is i rows and j columns older than the CURRENT
 *    scan position (i=j=0 = newest/current pixel). This anchor is a
 *    FIXED physical slot regardless of the runtime Kh/Kw -- the
 *    alternative (anchoring at "row Kh-1" so index Kh-1 is always
 *    current) would make the insert index itself runtime-dependent,
 *    reintroducing exactly the dynamic-index problem this rewrite
 *    exists to remove.
 *
 * 2. Because the window is anchored at the *newest* end, and taps run
 *    from oldest (kh=0, per the ONNX/PyTorch conv weight layout) to
 *    newest (kh=Kh-1), the weight loader (LOAD_DW_WT) loads into
 *    wt_buf[Kh-1-kh][Kw-1-kw] -- i.e. the kernel is loaded FLIPPED, the
 *    textbook "convolution = correlation with a flipped kernel"
 *    equivalence. This is what lets the hot-loop dot product read
 *    window[i][j] and wt_buf[i][j] with the SAME compile-time-unrolled
 *    index i,j and zero runtime offset. LOAD_DW_WT itself runs once per
 *    output channel (Kh*Kw iterations, not in the II=1 hot path), so
 *    the flip costs nothing extra.
 *
 * 3. Padding is not a special case. The scan runs over an *extended*
 *    raster (row -pad_h..Hin-1+pad_h, col -pad_w..Win-1+pad_w); any
 *    position outside the real [0,Hin)x[0,Win) rectangle is simply a
 *    streamed zero that still goes through the ordinary shift/push path.
 *    This also serves as the pipeline "fill" period: the window becomes
 *    structurally valid exactly DW_MAX_K pushes after scan start, which
 *    -- because the scan itself starts DW_MAX_K-1 positions before the
 *    first possible output trigger -- means line_buf's stale content
 *    from a PREVIOUS channel's scan is always fully flushed before it
 *    could ever be read. No explicit per-channel buffer reset is needed.
 *
 * 4. Column-stride triggering avoids a per-cycle divider. row-level
 *    stride (row_trigger/oh) is computed once per ROW (~Hext times per
 *    channel, division is fine there); column-level stride uses an
 *    incrementing/wrapping phase counter instead of `% stride_w` every
 *    cycle, since that division would sit inside the II=1 hot loop.
 *
 * 5. fpg (expand factor) is implemented by re-running the whole
 *    single-pass stream once per f, matching the old design's
 *    LOOP_DW_FPG behavior. Production and the existing testbench only
 *    ever use fpg=1 (the real network's stride>=2/fpg>1 dwconv calls run
 *    on the ARM CPU, not the FPGA), so this path is present for
 *    signature/testbench completeness, not performance-tuned.
 *
 * Trade-off vs the tile design: the dot product is fully spatially
 * unrolled (up to DW_MAX_K*DW_MAX_K=49 multiply-adds per output pixel,
 * one output per cycle once the pipeline is full) rather than the old
 * design's DW_TC=4-wide time-multiplexed MAC array. This is more DSPs
 * per instance (up to 49 vs 4) in exchange for eliminating the
 * addressing critical path -- an intentional space-for-time trade that
 * matches how streaming FPGA convolution engines are normally built, and
 * is expected to matter less on this LUT-bound (not DSP-bound) xc7z020
 * (see project memory: DSP-packing attempts were abandoned specifically
 * because DSPs were never the scarce resource here).
 *
 * 2026-08-10 update: isolated 200MHz P&R on the first cut of this design
 * (real Vivado, not HLS's own estimate) gave WNS=-0.318ns -- close to but
 * not yet at the old tile design's tuned best (-0.196~-0.261ns). The
 * worst path was NOT a recurrence of the dynamic-index problem this
 * rewrite targets; it was ap_start (of the LOOP_DW_COL pipeline's
 * per-row relaunch) feeding a wide (~30-bit, 3xCARRY4) comparator/
 * register for `iw_ext`/`Wext`'s loop-bound check. All the scan-position
 * bookkeeping (ih_ext/iw_ext/Hext/Wext/row_rel/col_rel/oh/ow/col_phase)
 * was originally plain `int` (32-bit) even though their real range is
 * under DW_MAX_H/W+pad (well under 256) -- narrowed below to `pos_t`
 * (12-bit, generous headroom over the ~94 max ever needed) using the
 * same "narrow local copy of a wide runtime parameter" technique this
 * project already validated once before (2026-08-07: CHin_n = ap_uint<10>
 * copy of CHin, WNS -0.943->-0.573ns on the old tile design's DATAFLOW
 * counter). Kh/Kw/stride_h/stride_w/pad_h/pad_w/Hin/Win are also copied
 * into narrow locals wherever they feed the hot per-pixel loop's
 * comparisons, so every operand on that path is uniformly narrow, not
 * just the specific register the report named.
 *
 * 2026-08-11 update: this pos_t narrowing brought isolated WNS from
 * -0.318ns to -0.236ns (122/68,971 failing endpoints, down from
 * 850/70,247) -- inside the old tile design's historical best range.
 * The new worst path is a different bottleneck again: `ap_CS_fsm_reg`
 * fanning into the HLS-generated 32-bit/36-cycle software divider for
 * Hout/Wout (runtime stride_h/stride_w prevent constant-folding to a
 * shift), through a high-fanout (~4500) control net. Not yet retuned --
 * this merged-build integration is testing whether -0.236ns's isolated
 * gain survives combination with the other 4 workers' shared m_axi
 * adapter, before spending more tuning effort on either path.
 *============================================================*/

#include "dwconv_worker.h"

/* See 2026-08-10 update above: narrow type for all scan-position
 * bookkeeping in the hot per-pixel loop. Range needed is under ~94
 * (DW_MAX_H/W=64 + up to a few pixels of padding); 12 bits (+-2048)
 * leaves generous headroom without dragging 32-bit comparators/adders
 * into the II=1 critical path. */
typedef ap_int<12> pos_t;

static act_t ch_out_buf[DW_MAX_H * DW_MAX_W];

static inline act_t dwconv_apply_act(acc_t val, int act_mode, int shift) {
#pragma HLS INLINE
    acc_t s = val >> shift;
    act_t r;  /* W8A4: clamp to symmetric 4-bit range -7..7 (matches
               * fastvit_ip_w8a4's Python quant convention). */
    if      (s >  7) r =  7;
    else if (s < -7) r = -7;
    else             r = (act_t)s;
    if (act_mode == ACT_RELU && r < 0) r = 0;
    return r;
}

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
#pragma HLS ARRAY_PARTITION variable=ch_out_buf cyclic factor=16 dim=1
#pragma HLS BIND_STORAGE variable=ch_out_buf type=RAM_1P impl=BRAM

    /* line_buf holds the DW_MAX_K-1 most recently *completed* input rows
     * (age 1..DW_MAX_K-1). line_buf[0] = age 1 (row just completed),
     * ..., line_buf[DW_MAX_K-2] = age DW_MAX_K-1 (oldest kept). The
     * *current* row being streamed (age 0) never touches line_buf -- it
     * feeds window[0][*] directly from the byte/nibble stream. Row dim
     * fully partitioned -> each age is its own small bank, addressed
     * only by the natural column counter `iw` (never a computed/
     * re-based tile offset). */
    static act_t line_buf[DW_MAX_K - 1][DW_MAX_W];
#pragma HLS ARRAY_PARTITION variable=line_buf complete dim=1

    /* window[i][j]: age-i row, age-j column of the current KxK tap set.
     * Always DW_MAX_K x DW_MAX_K physically; rows/cols beyond the
     * runtime Kh/Kw just carry stale shifted data the dot-product's
     * (i<Kh && j<Kw) guard ignores. Every index below is either a
     * compile-time-unrolled loop constant or +-1 of one -- never a
     * runtime-computed base+offset read (see design note 1 above). */
    act_t window[DW_MAX_K][DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=window complete dim=0

    /* Loaded FLIPPED -- see design note 2 above. */
    wt_t wt_buf[DW_MAX_K][DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=wt_buf complete dim=0

    int Hout = (Hin + 2*pad_h - Kh) / stride_h + 1;
    int Wout = (Win + 2*pad_w - Kw) / stride_w + 1;
    /* W8A4: 8 nibble-packed act_t lanes per 32-bit pack_t word. */
    int hw_in_words  = (Hin * Win)   >> 3;
    int hw_out_words = (Hout * Wout) >> 3;

    /* Narrow copies of every runtime parameter that feeds the hot
     * per-pixel loop's comparisons/arithmetic -- see 2026-08-10 note
     * above. The wide (int) originals are still used for the address/
     * word-count math just above, where 32-bit is harmless (computed
     * once per call, not per cycle). */
    pos_t Hin_n      = Hin;
    pos_t Win_n      = Win;
    pos_t Kh_n       = Kh;
    pos_t Kw_n       = Kw;
    pos_t stride_h_n = stride_h;
    pos_t stride_w_n = stride_w;
    pos_t pad_h_n    = pad_h;
    pos_t pad_w_n    = pad_w;
    pos_t Hout_n     = Hout;
    pos_t Wout_n     = Wout;
    pos_t Hext = Hin_n + 2 * pad_h_n;
    pos_t Wext = Win_n + 2 * pad_w_n;

    LOOP_DW_CH:
    for (int ch = 0; ch < CHin; ch++) {
#pragma HLS LOOP_TRIPCOUNT min=48 max=512

        LOOP_DW_FPG:
        for (int f = 0; f < fpg; f++) {
            int co = ch * fpg + f;

            LOAD_DW_WT:
            for (int kh = 0; kh < Kh; kh++) {
#pragma HLS PIPELINE II=1
                for (int kw = 0; kw < Kw; kw++) {
                    wt_buf[Kh - 1 - kh][Kw - 1 - kw] =
                        weight[co * Kh * Kw + kh * Kw + kw];
                }
            }

            acc_t bias_val = bias[co];

            /* col_phase tracks (iw_ext-(Kw-1)) mod stride_w incrementally
             * -- see design note 4 above. Computed once per call (not
             * per row: the extended column range is identical every
             * row, so the phase pattern repeats identically per row --
             * re-derived fresh at the top of each row below). */
            pos_t col_phase_init = ((-(Kw_n - 1)) % stride_w_n + stride_w_n) % stride_w_n;

            ap_uint<12> in_hw_idx = 0;
            ap_uint<3>  lane = 0;
            act_t lanes[8];
#pragma HLS ARRAY_PARTITION variable=lanes complete dim=1

            LOOP_DW_ROW:
            for (pos_t ih_ext = 0; ih_ext < Hext; ih_ext++) {
#pragma HLS LOOP_TRIPCOUNT min=16 max=80
                pos_t ih = ih_ext - pad_h_n;
                bool row_real = (ih >= 0 && ih < Hin_n);
                pos_t row_rel = ih_ext - (Kh_n - 1);
                bool row_trigger = (row_rel >= 0) && (row_rel % stride_h_n == 0);
                pos_t oh = row_trigger ? (pos_t)(row_rel / stride_h_n) : (pos_t)0;

                pos_t col_phase = col_phase_init;
                pos_t ow = 0;

                LOOP_DW_COL:
                for (pos_t iw_ext = 0; iw_ext < Wext; iw_ext++) {
#pragma HLS PIPELINE II=1
                    pos_t iw = iw_ext - pad_w_n;
                    bool col_real = (iw >= 0 && iw < Win_n);

                    act_t pixel = 0;
                    if (row_real && col_real) {
                        if (lane == 0) {
                            pack_t word = feat_in[ch * hw_in_words + in_hw_idx];
                            lanes[0] = (act_t)word.range( 3,  0);
                            lanes[1] = (act_t)word.range( 7,  4);
                            lanes[2] = (act_t)word.range(11,  8);
                            lanes[3] = (act_t)word.range(15, 12);
                            lanes[4] = (act_t)word.range(19, 16);
                            lanes[5] = (act_t)word.range(23, 20);
                            lanes[6] = (act_t)word.range(27, 24);
                            lanes[7] = (act_t)word.range(31, 28);
                            in_hw_idx++;
                        }
                        pixel = lanes[lane];
                        lane = (lane == 7) ? (ap_uint<3>)0 : (ap_uint<3>)(lane + 1);
                    }

                    /* New age-0 column, per row: row 0 comes straight
                     * from the stream; rows 1..DW_MAX_K-1 come from
                     * line_buf's PRE-update (this cycle's old) content.
                     * Padding columns contribute zero to every row. */
                    act_t new_col[DW_MAX_K];
#pragma HLS ARRAY_PARTITION variable=new_col complete dim=1
                    new_col[0] = pixel;
                    LOOP_ROWHIST:
                    for (int i = 1; i < DW_MAX_K; i++) {
#pragma HLS UNROLL
                        new_col[i] = col_real ? line_buf[i - 1][iw] : (act_t)0;
                    }

                    /* Shift window right (age increases with column
                     * index), then insert the new age-0 column. */
                    LOOP_WIN_ROW:
                    for (int i = 0; i < DW_MAX_K; i++) {
#pragma HLS UNROLL
                        LOOP_WIN_COL:
                        for (int j = DW_MAX_K - 1; j > 0; j--) {
#pragma HLS UNROLL
                            window[i][j] = window[i][j - 1];
                        }
                        window[i][0] = new_col[i];
                    }

                    /* line_buf age update (shift ages up by one); only
                     * for real columns -- line_buf has no storage for
                     * padding columns and none is needed (see note 3). */
                    if (col_real) {
                        LOOP_LINEBUF_SHIFT:
                        for (int i = DW_MAX_K - 2; i > 0; i--) {
#pragma HLS UNROLL
                            line_buf[i][iw] = line_buf[i - 1][iw];
                        }
                        line_buf[0][iw] = pixel;
                    }

                    pos_t col_rel = iw_ext - (Kw_n - 1);
                    bool col_trigger = (col_rel >= 0) && (col_phase == 0);

                    if (row_trigger && col_trigger && oh < Hout_n && ow < Wout_n) {
                        acc_t sum = bias_val;
                        LOOP_DOT_I:
                        for (int i = 0; i < DW_MAX_K; i++) {
#pragma HLS UNROLL
                            LOOP_DOT_J:
                            for (int j = 0; j < DW_MAX_K; j++) {
#pragma HLS UNROLL
                                if (i < Kh && j < Kw) {
                                    sum += (acc_t)window[i][j] * (acc_t)wt_buf[i][j];
                                }
                            }
                        }
                        /* Wide (int) Wout deliberately used here, not
                         * Wout_n -- Hout*Wout can reach 4096, which
                         * overflows signed pos_t's (ap_int<12>) +-2047
                         * range. This address multiply is not on the
                         * flagged critical path, so there's no timing
                         * reason to narrow it. */
                        ch_out_buf[(int)oh * Wout + (int)ow] =
                            dwconv_apply_act(sum, act_mode, out_shift);
                        ow++;
                    }

                    col_phase = (col_phase == stride_w_n - 1) ? (pos_t)0 : (pos_t)(col_phase + 1);
                } // col
            } // row

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
