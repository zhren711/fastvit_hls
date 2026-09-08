// dw_raster_layer.cpp -- see dw_raster_layer.h for the fpg design and
// scope notes.
//
// Fix 1 (kept, unconditional) -- total_beats = h_pad*w_pad used to bind
// to mul_32s_32s_32_2_1, the SAME shared-multiplier core CLAUDE.md
// documents on this design's critical path (WNS=+0.003ns). Removed by
// mirroring dwr_produce's own nested ROW/COL loop structure in consume
// instead of flattening to one total_beats-bounded loop -- h_pad/w_pad
// are only ever loop bounds now, never multiplied. Confirmed via the
// .bind.adb database: zero occurrences of mul_32s_32s_32_2_1 anywhere in
// this design after the fix (grep across every *.adb in the csynth
// project, not just this file's own report).
//
// Fix 2 (DWR_ENABLE_FPG_SPECIALIZATION, OFF by default) -- template<int
// FPG_MODE> dwr_consume_impl<> giving fpg=1 layers their own true-II=1
// circuit instead of sharing dwr_consume's II=2 with fpg=2. Tried and
// measured: real DSP cost is 206/220 (93.6%), not the 148/220 this
// project's own margin-recovery heuristic was written against, because
// the two template instantiations are separate RTL modules/FSMs (same
// finding as PW_FLAT's own rejected INLINE-off-sharing attempt -- HLS
// resource sharing only happens within one function's own FSM, not
// across two statically-dispatched instantiations) -- there is no way to
// share the 49*fpg-tap reduction between them without also bringing back
// the single-shared-circuit II=2 problem Fix 2 exists to solve. Given
// this design's WNS is already +0.003ns and this project has hit a
// >=95%-BRAM unroutable-pins failure before at high occupancy, 93.6% DSP
// with only 14 spare is rejected -- kept here, #ifdef'd off, for when
// DSP budget genuinely allows it (e.g. after DSP-packing lands and some
// of this design's own DSP usage shrinks elsewhere).
//
// Default path (DWR_ENABLE_FPG_SPECIALIZATION undefined): single shared
// dwr_consume(), DWR_MAX_FPG=2 always statically unrolled with a
// g_valid gate (same clamp-to-safe-constant convention as the K<MAX_K
// taps), II=2 for EVERY layer including the 21 real fpg=1 ones. Real,
// registered cost: those 21 layers each run at half their achievable
// per-channel rate (1 output every 2 cycles instead of 1/cycle) with no
// compensating benefit, since g=1 never fires for them. Accepted this
// round because: DW is 28% of total network time, and raster already
// beats the current tile-based architecture by ~39-49x even before this
// tax -- there's ample margin to spend, and DSP/LUT/WNS margin currently
// is not.
#include "dw_raster_layer.h"
#include <hls_stream.h>

struct dwr_beat_t {
    act_t window[DWR_MAX_K][DWR_MAX_K];
    bool  valid;
};

#ifdef DWR_INPUT_BURST
// ZHR-92 round (2026-09-07): DWR_INPUT_BURST (OFF by default) -- ATTEMPTED
// AND REJECTED, real board result, kept here per this file's own
// established convention (see DWR_ENABLE_FPG_SPECIALIZATION/LB_FORCE_DSP/
// DWR_HOIST_BASE_ADDR above) rather than deleted.
//
// Hypothesis: a GELU/ADD-style decomposition found DW's real-to-naive-
// floor ratio was 18.80x (7.48x once corrected for padding+achieved II),
// matching GELU's own 8.60x -- attributed to dwr_produce's per-pixel
// in_base[] read (one un-bursted AXI transaction per pixel), the same
// mechanism ELEMWISE_BURST had just fixed for GELU/ADD at a real -93%/
// -91% win. Implementation: dwr_prefetch_channel() bursts a whole
// channel's real (unpadded) plane via hls::burst_maxi<ap_uint<32>>
// (dw_in_burst, chunked at ELEMWISE_CHUNK_WORDS) into ch_buf BEFORE this
// function's own PIPELINE II=1 COL loop starts, which then reads
// ch_buf[read_ptr] instead of in_base[ci_base+read_ptr].
//
// Real P&R: WNS +0.004ns after one phys_opt_design pass (route_design
// alone: -0.103ns) -- itself in this project's own "<+0.01ns, matches the
// already-rejected +0.0036ns precedent" non-deliverable band, but board-
// tested anyway per explicit instruction (measurement, not deployment;
// two full-network runs came back byte-identical across all 6 checkpoints
// + entry81, ruling out a marginal-timing artifact).
//
// REAL BOARD RESULT: DW got WORSE, not better -- 531.20ms (baseline) ->
// 574.22ms (+8.1%), full network 1,099.77ms -> ~1,138.87ms (+3.5%). PW/
// GELU/ADD unaffected (within noise); all 6 ONNX cosine checkpoints exact
// match, correctness fully preserved.
//
// ROOT CAUSE (found by redoing the original decomposition's own floor
// calculation with a number this SAME round's csynth had already
// measured but hadn't been used to correct it): the original 7.48x
// "adjusted floor" used this file's own header comment claiming
// dwr_consume's achieved II=2 -- but csynth (checked for an unrelated
// item, whether this round's change regressed it) confirmed the REAL
// achieved II is 8, unchanged from the baseline, not introduced by this
// round. Recomputed with II=8: the baseline's real ratio was already only
// 1.87x, not 7.48x -- dwr_consume's own reduction/writeout pipeline, not
// the input read, is DW's real bottleneck, and it was already close to
// its own floor. This build's ratio came back 2.02x, slightly worse than
// baseline. This is the SAME mechanism CLAUDE.md already documents for
// output-write batching (PW_WRITEOUT_FLUSH): pulling an access out of an
// already-pipelined, latency-hidden context into its own serial pre-stage
// makes its real per-transaction cost fully additive instead of hidden --
// here compounded by optimizing the wrong side of the pipeline entirely,
// since consume's own II=8 (not the input read) was always the binding
// constraint. Practical consequence: any future DW timing work on this
// line should target dwr_consume's own II=8, not the input read.
static void dwr_produce_burst(
    const act_t ch_buf[DWR_CH_BUF_BYTES],
    int h_in, int w_in, int K, int S,
    hls::stream<dwr_beat_t> &taps)
{
    act_t lb0[DWR_WPAD_MAX], lb1[DWR_WPAD_MAX], lb2[DWR_WPAD_MAX];
    act_t lb3[DWR_WPAD_MAX], lb4[DWR_WPAD_MAX], lb5[DWR_WPAD_MAX];

    act_t window[DWR_MAX_K][DWR_MAX_K];
#pragma HLS ARRAY_PARTITION variable=window complete dim=0

    const int pad   = K / 2;
    const int h_pad = h_in + 2 * pad;
    const int w_pad = w_in + 2 * pad;

    int read_ptr = 0;
    int row_phase = 0;

    ROW: for (int prow = 0; prow < h_pad; prow++) {
        int real_row = prow - pad;
        bool row_in_image = (real_row >= 0) && (real_row < h_in);

        bool row_valid;
        if (prow < K - 1) {
            row_valid = false;
        } else if (prow == K - 1) {
            row_phase = 0;
            row_valid = true;
        } else {
            row_phase++;
            if (row_phase == S) row_phase = 0;
            row_valid = (row_phase == 0);
        }

        int col_phase = 0;

        COL: for (int pcol = 0; pcol < w_pad; pcol++) {
#pragma HLS PIPELINE II=1
            int real_col = pcol - pad;
            bool col_in_image = row_in_image && (real_col >= 0) && (real_col < w_in);

            bool col_valid;
            if (pcol < K - 1) {
                col_valid = false;
            } else if (pcol == K - 1) {
                col_phase = 0;
                col_valid = true;
            } else {
                col_phase++;
                if (col_phase == S) col_phase = 0;
                col_valid = (col_phase == 0);
            }

            dwr_beat_t beat;
            beat.valid = row_valid && col_valid;

            act_t new_pixel = col_in_image ? ch_buf[read_ptr] : (act_t)0;

            act_t v5 = lb5[pcol]; lb5[pcol] = new_pixel;
            act_t v4 = lb4[pcol]; lb4[pcol] = v5;
            act_t v3 = lb3[pcol]; lb3[pcol] = v4;
            act_t v2 = lb2[pcol]; lb2[pcol] = v3;
            act_t v1 = lb1[pcol]; lb1[pcol] = v2;
            act_t v0 = lb0[pcol]; lb0[pcol] = v1;

            for (int r = 0; r < DWR_MAX_K; r++)
                for (int c = 0; c < DWR_MAX_K - 1; c++)
                    window[r][c] = window[r][c + 1];
            window[0][DWR_MAX_K - 1] = v0;
            window[1][DWR_MAX_K - 1] = v1;
            window[2][DWR_MAX_K - 1] = v2;
            window[3][DWR_MAX_K - 1] = v3;
            window[4][DWR_MAX_K - 1] = v4;
            window[5][DWR_MAX_K - 1] = v5;
            window[6][DWR_MAX_K - 1] = new_pixel;

            for (int r = 0; r < DWR_MAX_K; r++)
                for (int c = 0; c < DWR_MAX_K; c++)
                    beat.window[r][c] = window[r][c];
            taps.write(beat);

            if (col_in_image) read_ptr++;
        }
    }
}
#else
// Default path (DWR_INPUT_BURST undefined): direct in_base[] read, one
// pixel per valid cycle inside this function's own PIPELINE II=1 COL
// loop -- unchanged from before the DWR_INPUT_BURST attempt above. Real
// board result confirmed this default path is not the mechanism worth
// fixing (see DWR_INPUT_BURST's own comment) -- dwr_consume's achieved
// II=8 is the real bottleneck, untouched by this file's own input side.
static void dwr_produce(
    const act_t in_base[], int in_off, int ci, int in_ch_stride,
    int h_in, int w_in, int K, int S,
    hls::stream<dwr_beat_t> &taps)
{
    act_t lb0[DWR_WPAD_MAX], lb1[DWR_WPAD_MAX], lb2[DWR_WPAD_MAX];
    act_t lb3[DWR_WPAD_MAX], lb4[DWR_WPAD_MAX], lb5[DWR_WPAD_MAX];

    act_t window[DWR_MAX_K][DWR_MAX_K];
#pragma HLS ARRAY_PARTITION variable=window complete dim=0

    const int pad   = K / 2;
    const int h_pad = h_in + 2 * pad;
    const int w_pad = w_in + 2 * pad;
    const int ci_base = in_off + ci * in_ch_stride;

    int read_ptr = 0;
    int row_phase = 0;

    ROW: for (int prow = 0; prow < h_pad; prow++) {
        int real_row = prow - pad;
        bool row_in_image = (real_row >= 0) && (real_row < h_in);

        bool row_valid;
        if (prow < K - 1) {
            row_valid = false;
        } else if (prow == K - 1) {
            row_phase = 0;
            row_valid = true;
        } else {
            row_phase++;
            if (row_phase == S) row_phase = 0;
            row_valid = (row_phase == 0);
        }

        int col_phase = 0;

        COL: for (int pcol = 0; pcol < w_pad; pcol++) {
#pragma HLS PIPELINE II=1
            int real_col = pcol - pad;
            bool col_in_image = row_in_image && (real_col >= 0) && (real_col < w_in);

            bool col_valid;
            if (pcol < K - 1) {
                col_valid = false;
            } else if (pcol == K - 1) {
                col_phase = 0;
                col_valid = true;
            } else {
                col_phase++;
                if (col_phase == S) col_phase = 0;
                col_valid = (col_phase == 0);
            }

            dwr_beat_t beat;
            beat.valid = row_valid && col_valid;

            act_t new_pixel = col_in_image ? in_base[ci_base + read_ptr] : (act_t)0;

            act_t v5 = lb5[pcol]; lb5[pcol] = new_pixel;
            act_t v4 = lb4[pcol]; lb4[pcol] = v5;
            act_t v3 = lb3[pcol]; lb3[pcol] = v4;
            act_t v2 = lb2[pcol]; lb2[pcol] = v3;
            act_t v1 = lb1[pcol]; lb1[pcol] = v2;
            act_t v0 = lb0[pcol]; lb0[pcol] = v1;

            for (int r = 0; r < DWR_MAX_K; r++)
                for (int c = 0; c < DWR_MAX_K - 1; c++)
                    window[r][c] = window[r][c + 1];
            window[0][DWR_MAX_K - 1] = v0;
            window[1][DWR_MAX_K - 1] = v1;
            window[2][DWR_MAX_K - 1] = v2;
            window[3][DWR_MAX_K - 1] = v3;
            window[4][DWR_MAX_K - 1] = v4;
            window[5][DWR_MAX_K - 1] = v5;
            window[6][DWR_MAX_K - 1] = new_pixel;

            for (int r = 0; r < DWR_MAX_K; r++)
                for (int c = 0; c < DWR_MAX_K; c++)
                    beat.window[r][c] = window[r][c];
            taps.write(beat);

            if (col_in_image) read_ptr++;
        }
    }
}
#endif // DWR_INPUT_BURST

// FAST path: 4 consecutive already-valid positions of the SAME channel,
// packed into one contiguous store. SLOW path: the <4-element remainder
// at the very end of a channel's output plane. Both write the identical
// bytes to the identical addresses.
template<bool FAST>
static void dwr_writeout_impl(act_t out_base[], int base_addr, int start, act_t vals[4], int n)
{
    for (int i = 0; i < n; i++) {
        out_base[base_addr + start + i] = vals[i];
    }
}

#ifdef DWR_ENABLE_FPG_SPECIALIZATION
// Option A -- ORIGINAL CLAIM (superseded, see correction below): true
// II=1 for fpg=1, but real measured cost is 93.6% DSP (206/220) on this
// chip once both instantiations coexist -- rejected that round (WNS=
// +0.003ns, and this project's own >=95%-BRAM unroutable-pins
// precedent). Kept for when DSP budget allows it.
//
// CORRECTION, 2026-09-01 (ZHR-92/ZHR-63, PW tiling "make room" round):
// RE-MEASURED, does NOT reproduce. Fresh isolated csynth (this file
// unchanged in its own FPG-specialization logic, but several other
// rounds touched this file since Option A was last measured --
// DWR_HOIST_BASE_ADDR, LB_FORCE_DSP, the writeout template<bool> split
// -- none independently re-verified against this flag until now) shows
// BOTH instantiations regressed, not just FPG_MODE=2: run_dw_layer_
// raster's own isolated LUT went 11,543 -> 28,017 (+143%, the opposite
// of the intended saving), and CROW_CCOL's achieved II is 4 for
// FPG_MODE=1 (not the claimed II=1) and 8 for FPG_MODE=2 -- both worse
// than the deployed default's own II=1. csim stays clean (wiring tb
// 4/4, dw_raster_layer_tb 5/5 including 2 real fpg=2 shapes), so this
// is a real synthesis-quality regression, not a correctness one. Root
// cause NOT investigated this round (closing this whole line, not
// worth the further rounds a root-cause chase would cost -- see ZHR-63
// PW-tiling closeout). This is the highest-cost instance yet of this
// project's own "a comment asserting a test result needs re-
// verification before being relied on" lesson -- an entire round's
// experiment design (ZHR-92, 2026-09-01) was built on this stale
// number before it was ever re-checked. Do not cite the "93.6% DSP,
// true II=1" claim above without re-running this flag fresh first.
template<int FPG_MODE>
static void dwr_consume_impl(
    const wt_t w_base[], int w_off, int shift_off, const acc_t b_base[], int b_off,
    int ci, int fpg, int K,
    act_t out_base[], int out_off, int out_ch_stride,
    int h_pad, int w_pad,
    hls::stream<dwr_beat_t> &taps)
{
    wt_t weight_aligned[FPG_MODE][DWR_MAX_K][DWR_MAX_K];
#pragma HLS ARRAY_PARTITION variable=weight_aligned complete dim=0
    acc_t bias[FPG_MODE];
    int   shift[FPG_MODE];
#ifdef DWR_HOIST_BASE_ADDR
    // see dwr_consume's own DWR_HOIST_BASE_ADDR comment -- same fix,
    // same OFF-by-default reasoning (real P&R'd worse despite flat/
    // improved resources on the Option B build; never independently
    // re-verified against this Option A build specifically).
    int base_addr_arr[FPG_MODE];
#endif

    const int off = DWR_MAX_K - K;
    for (int g = 0; g < FPG_MODE; g++) {
        int co = ci * fpg + g;  // FPG_MODE == fpg by construction (dispatch below)
        bias[g]  = b_base[b_off + co];
        shift[g] = (int)w_base[shift_off + co];
#ifdef DWR_HOIST_BASE_ADDR
        base_addr_arr[g] = out_off + co * out_ch_stride;
#endif
        for (int kh = 0; kh < K; kh++)
            for (int kw = 0; kw < K; kw++)
                weight_aligned[g][off + kh][off + kw] = w_base[w_off + co * K * K + kh * K + kw];
    }

    act_t wbuf[FPG_MODE][4];
    int   wbuf_n[FPG_MODE];
    int   write_ptr[FPG_MODE];
    for (int g = 0; g < FPG_MODE; g++) { wbuf_n[g] = 0; write_ptr[g] = 0; }

    CROW: for (int prow = 0; prow < h_pad; prow++) {
        CCOL: for (int pcol = 0; pcol < w_pad; pcol++) {
#pragma HLS PIPELINE II=1
            dwr_beat_t beat = taps.read();
            if (beat.valid) {
                for (int g = 0; g < FPG_MODE; g++) {
                    acc_t sum = 0;
                    for (int r = 0; r < DWR_MAX_K; r++) {
                        for (int c = 0; c < DWR_MAX_K; c++) {
                            bool active = (r >= off) && (c >= off);
                            acc_t prod;
                            /* NOT ADOPTED: forced-DSP real P&R failed
                             * timing (WNS=-0.108ns) vs. the default
                             * LUT-mode build meeting it (WNS=+0.021ns) on
                             * matching resource totals -- see CLAUDE.md's
                             * "HLS-level resource-binding choice ...
                             * does not reliably determine ... resource
                             * distribution, but can still change real
                             * placement/timing" entry. LB_FORCE_DSP stays
                             * undefined by default; run_csim_gmem_meta_
                             * elim.tcl documents the deployed baseline's
                             * build flags explicitly for this reason. */
#ifdef LB_FORCE_DSP
#pragma HLS BIND_OP variable=prod op=mul impl=DSP
#endif
                            prod = (acc_t)beat.window[r][c] * (acc_t)weight_aligned[g][r][c];
                            sum += active ? prod : (acc_t)0;
                        }
                    }
                    acc_t total = sum + bias[g];
                    acc_t v = total >> shift[g];
                    if (v > 127)  v = 127;
                    if (v < -128) v = -128;

#ifdef DWR_HOIST_BASE_ADDR
                    int base_addr = base_addr_arr[g];
#else
                    int co2 = ci * fpg + g;
                    int base_addr = out_off + co2 * out_ch_stride;
#endif

                    wbuf[g][wbuf_n[g]] = (act_t)v;
                    wbuf_n[g]++;
                    if (wbuf_n[g] == 4) {
                        dwr_writeout_impl<true>(out_base, base_addr, write_ptr[g], wbuf[g], 4);
                        write_ptr[g] += 4;
                        wbuf_n[g] = 0;
                    }
                }
            }
        }
    }
    for (int g = 0; g < FPG_MODE; g++) {
        if (wbuf_n[g] > 0) {
#ifdef DWR_HOIST_BASE_ADDR
            int base_addr = base_addr_arr[g];
#else
            int co2 = ci * fpg + g;
            int base_addr = out_off + co2 * out_ch_stride;
#endif
            dwr_writeout_impl<false>(out_base, base_addr, write_ptr[g], wbuf[g], wbuf_n[g]);
            write_ptr[g] += wbuf_n[g];
        }
    }
}
#else
// Option B (default) -- single shared circuit, DWR_MAX_FPG=2 always
// statically unrolled, g_valid-gated (same clamp-to-safe-constant
// convention as the K<MAX_K taps: co clamps to ci*fpg+0 when g is out of
// range for this layer's real fpg, so an fpg=1 layer's g=1 lane never
// reads past its own channel's weight/bias/shift region). Real,
// registered cost: II=2 for every layer, including the 21 real fpg=1
// ones -- see the file-header comment for why this is accepted this
// round (DW's 39-49x headroom over the tile-based baseline absorbs it;
// DSP/LUT/WNS margin currently can't).
static void dwr_consume(
    const wt_t w_base[], int w_off, int shift_off, const acc_t b_base[], int b_off,
    int ci, int fpg, int K,
    act_t out_base[], int out_off, int out_ch_stride,
    int h_pad, int w_pad,
    hls::stream<dwr_beat_t> &taps)
{
    wt_t weight_aligned[DWR_MAX_FPG][DWR_MAX_K][DWR_MAX_K];
#pragma HLS ARRAY_PARTITION variable=weight_aligned complete dim=0
    acc_t bias[DWR_MAX_FPG];
    int   shift[DWR_MAX_FPG];
#ifdef DWR_HOIST_BASE_ADDR
    // ZHR-92 Phase 1 Step 2 P&R round (2026-08-28): `co`/`base_addr` are
    // loop-invariant per (ci,g) across the whole CROW/CCOL pass -- hoisted
    // here into a once-per-channel precompute instead of recomputed every
    // valid beat. Correct fix (same bug class as Fix 1, found by real P&R
    // -- a core-name collision with pw_flat_pipeline's own
    // mul_32s_32s_32_2_1 that no isolated csynth check could have caught;
    // a full rescan of every `*` in this file confirmed nothing else of
    // this class remains), and it measurably does NOT cost resources
    // (LUT flat, DSP -6 in real P&R) -- but on THIS design's current
    // placement it real-P&R'd to WNS -0.355ns, worse than leaving the
    // "obviously less efficient" per-beat recompute in place (WNS
    // +0.021ns, the only real-P&R-passing build so far). Third confirmed
    // instance on this line of work that a structurally-correct change
    // can still lose on real placement even at flat/improved resource
    // cost -- see CLAUDE.md. OFF by default: re-evaluate when this
    // design's placement margin has more room to absorb a structural
    // change, not assumed to always help just because it's the more
    // efficient code.
    int base_addr_arr[DWR_MAX_FPG];
#endif

    const int off = DWR_MAX_K - K;
    for (int g = 0; g < DWR_MAX_FPG; g++) {
        bool g_valid = (g < fpg);
        int co = ci * fpg + (g_valid ? g : 0);
        bias[g]  = b_base[b_off + co];
        shift[g] = (int)w_base[shift_off + co];
#ifdef DWR_HOIST_BASE_ADDR
        base_addr_arr[g] = out_off + co * out_ch_stride;
#endif
        for (int kh = 0; kh < K; kh++)
            for (int kw = 0; kw < K; kw++)
                weight_aligned[g][off + kh][off + kw] = w_base[w_off + co * K * K + kh * K + kw];
    }

    act_t wbuf[DWR_MAX_FPG][4];
    int   wbuf_n[DWR_MAX_FPG];
    int   write_ptr[DWR_MAX_FPG];
    for (int g = 0; g < DWR_MAX_FPG; g++) { wbuf_n[g] = 0; write_ptr[g] = 0; }

    // Fix 1 kept here too: nested ROW/COL, mirroring dwr_produce, instead
    // of a total_beats(=h_pad*w_pad)-bounded flat loop.
    CROW: for (int prow = 0; prow < h_pad; prow++) {
        CCOL: for (int pcol = 0; pcol < w_pad; pcol++) {
#pragma HLS PIPELINE II=1
            dwr_beat_t beat = taps.read();
            if (beat.valid) {
                for (int g = 0; g < DWR_MAX_FPG; g++) {
                    bool g_valid = (g < fpg);
                    acc_t sum = 0;
                    for (int r = 0; r < DWR_MAX_K; r++) {
                        for (int c = 0; c < DWR_MAX_K; c++) {
                            bool active = (r >= off) && (c >= off);
                            acc_t prod;
                            /* NOT ADOPTED: forced-DSP real P&R failed
                             * timing (WNS=-0.108ns) vs. the default
                             * LUT-mode build meeting it (WNS=+0.021ns) on
                             * matching resource totals -- see CLAUDE.md's
                             * "HLS-level resource-binding choice ...
                             * does not reliably determine ... resource
                             * distribution, but can still change real
                             * placement/timing" entry. LB_FORCE_DSP stays
                             * undefined by default; run_csim_gmem_meta_
                             * elim.tcl documents the deployed baseline's
                             * build flags explicitly for this reason. */
#ifdef LB_FORCE_DSP
#pragma HLS BIND_OP variable=prod op=mul impl=DSP
#endif
                            prod = (acc_t)beat.window[r][c] * (acc_t)weight_aligned[g][r][c];
                            sum += active ? prod : (acc_t)0;
                        }
                    }
                    acc_t total = sum + bias[g];
                    acc_t v = total >> shift[g];
                    if (v > 127)  v = 127;
                    if (v < -128) v = -128;

#ifdef DWR_HOIST_BASE_ADDR
                    int base_addr = base_addr_arr[g];
#else
                    // default: real-P&R-passing build (WNS +0.021ns) --
                    // recomputed every valid beat, see the
                    // DWR_HOIST_BASE_ADDR comment above for why this
                    // "less efficient" form is the one currently deployed.
                    int co = ci * fpg + (g_valid ? g : 0);
                    int base_addr = out_off + co * out_ch_stride;
#endif

                    if (g_valid) {
                        wbuf[g][wbuf_n[g]] = (act_t)v;
                        wbuf_n[g]++;
                        if (wbuf_n[g] == 4) {
                            dwr_writeout_impl<true>(out_base, base_addr, write_ptr[g], wbuf[g], 4);
                            write_ptr[g] += 4;
                            wbuf_n[g] = 0;
                        }
                    }
                }
            }
        }
    }
    for (int g = 0; g < DWR_MAX_FPG; g++) {
        bool g_valid = (g < fpg);
        if (g_valid && wbuf_n[g] > 0) {
#ifdef DWR_HOIST_BASE_ADDR
            int base_addr = base_addr_arr[g];
#else
            int co = ci * fpg + g;
            int base_addr = out_off + co * out_ch_stride;
#endif
            dwr_writeout_impl<false>(out_base, base_addr, write_ptr[g], wbuf[g], wbuf_n[g]);
            write_ptr[g] += wbuf_n[g];
        }
    }
}
#endif

#ifdef DWR_INPUT_BURST
// ZHR-92 round (2026-09-07): whole-channel prefetch, deliberately OUTSIDE
// any #pragma HLS DATAFLOW region (see run_dw_layer_raster below) --
// bursts this channel's real (unpadded) plane from dw_in_burst
// (bundle=gmem_act, same bundle as in_base) into ch_buf, chunked at
// ELEMWISE_CHUNK_WORDS (1024 words/4KB), mirroring ELEMWISE_BURST's own
// chunk size exactly. n_bytes is always mod4==0 for every real DW layer
// (verified before implementing, not assumed -- see this file's own
// header comment and DWR_CH_BUF_BYTES's comment in dw_raster_layer.h),
// so no tail-byte handling is needed: every word maps to exactly 4 valid
// bytes, unlike ELEMWISE_BURST's own (also-clean) mod4 case which still
// carried defensive bounds per the same verification discipline.
static void dwr_prefetch_channel(
    hls::burst_maxi<ap_uint<32> > dw_in_burst,
    int in_off, int ci, int in_ch_stride, int h_in, int w_in,
    act_t ch_buf[DWR_CH_BUF_BYTES])
{
    const int n_bytes = h_in * w_in;
    const int n_words  = n_bytes >> 2;
    const int base_w   = (in_off + ci * in_ch_stride) >> 2;

    DWR_PREFETCH_CHUNK: for (int base = 0; base < n_words; base += ELEMWISE_CHUNK_WORDS) {
        int this_chunk = (n_words - base < ELEMWISE_CHUNK_WORDS) ? (n_words - base) : ELEMWISE_CHUNK_WORDS;
        dw_in_burst.read_request(base_w + base, this_chunk);
        DWR_PREFETCH_READ: for (int i = 0; i < ELEMWISE_CHUNK_WORDS; i++) {
#pragma HLS PIPELINE II=1
            if (i < this_chunk) {
                ap_uint<32> w = dw_in_burst.read();
                int byte_base = (base + i) * 4;
                for (int k = 0; k < 4; k++) {
#pragma HLS UNROLL
                    ch_buf[byte_base + k] = (act_t)w.range(k * 8 + 7, k * 8);
                }
            }
        }
    }
}
#endif // DWR_INPUT_BURST

void run_dw_layer_raster(
    const act_t in_base[],
    const wt_t  w_base[],
    const acc_t b_base[],
    act_t        out_base[],
    hls::burst_maxi<ap_uint<32> > dw_in_burst,
    int cin, int cout, int h_in, int w_in,
    int K, int S, int pad, int fpg,
    int in_off, int w_off, int b_off, int out_off,
    int shift_off,
    int in_ch_stride, int out_ch_stride, int h_out, int w_out)
{
    (void)cout; (void)pad;
#ifndef DWR_INPUT_BURST
    (void)dw_in_burst;
#endif
    const int p = K / 2;
    const int h_pad = h_in + 2 * p;
    const int w_pad = w_in + 2 * p;

#ifdef DWR_INPUT_BURST
    act_t ch_buf[DWR_CH_BUF_BYTES];
#endif

    for (int ci = 0; ci < cin; ci++) {
#ifdef DWR_INPUT_BURST
        dwr_prefetch_channel(dw_in_burst, in_off, ci, in_ch_stride, h_in, w_in, ch_buf);
#endif

        hls::stream<dwr_beat_t> taps;
#pragma HLS STREAM variable=taps depth=2
#pragma HLS DATAFLOW
#ifdef DWR_INPUT_BURST
        dwr_produce_burst(ch_buf, h_in, w_in, K, S, taps);
#else
        dwr_produce(in_base, in_off, ci, in_ch_stride, h_in, w_in, K, S, taps);
#endif
#ifdef DWR_ENABLE_FPG_SPECIALIZATION
        if (fpg == 1) {
            dwr_consume_impl<1>(w_base, w_off, shift_off, b_base, b_off, ci, fpg, K,
                                 out_base, out_off, out_ch_stride, h_pad, w_pad, taps);
        } else {
            dwr_consume_impl<2>(w_base, w_off, shift_off, b_base, b_off, ci, fpg, K,
                                 out_base, out_off, out_ch_stride, h_pad, w_pad, taps);
        }
#else
        dwr_consume(w_base, w_off, shift_off, b_base, b_off, ci, fpg, K,
                    out_base, out_off, out_ch_stride, h_pad, w_pad, taps);
#endif
    }
}
