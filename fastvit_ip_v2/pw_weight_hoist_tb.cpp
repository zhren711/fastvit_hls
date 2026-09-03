/*============================================================
 * pw_weight_hoist_tb.cpp -- ZHR-92 (2026-09-02): dedicated PW-only csim for
 * the PW_WEIGHT_HOIST/pw_weight_cache mechanism. PW_CACHED is a plain
 * runtime bool (not a template dimension, see mac_array_raster_
 * integrated.cpp's own header comments for why) in mac_array_raster_
 * integrated.cpp.
 *
 * A same-day 2nd round tried raising PW_WEIGHT_CACHE_ELEMS to 442,368
 * (432KB, covering all 26 real PW layers at once) -- real P&R came back
 * BRAM 140/140 (100.00%, zero margin) and WNS=-1.075730ns (real
 * violation), so it was reverted back to 147,456 (144KB) the same day.
 * See mac_array_raster_integrated.cpp's own header comment at
 * PW_WEIGHT_CACHE_ELEMS for the full writeup.
 *
 * UPDATED 2026-09-03 (chunked weight loading): the 144KB cutoff no longer
 * gates a direct-DRAM-read fallback -- layers whose weight exceeds it are
 * now served via the PW_WCHUNK loop (multiple <=144KB loads instead of
 * one, see mac_array_raster_integrated.cpp's own header comment on
 * PW_WEIGHT_CACHE_ELEMS/pw_cached_ok). Cases 3/4 below (layer_0043/44,
 * the two real layers that USED TO exercise the direct-read fallback) now
 * exercise the CHUNKED path instead (3 exact chunks each) -- their own
 * semantics changed even though their shapes didn't; renamed accordingly.
 * Case 6 (previously the only case reaching the uncached/direct-read
 * branch) is retargeted to exercise chunking's UNEVEN-last-chunk case
 * instead (matching layer_0047/48's real 384+384+192-shaped split) --
 * the true uncached/direct-read branch is no longer reachable by any
 * shape this project can construct within MAX_CIN=1152 (would need
 * cin>147,456), so it is untested here, matching its new dead-code status
 * (kept, not deleted, per mac_array_raster_integrated.cpp's own comment).
 *
 * Neither existing testbench covers this: mac_array_tb.cpp's full 17-phase
 * suite CANNOT run against mac_array_raster_integrated.cpp at all (confirmed
 * fresh 2026-09-02 -- it aborts via assert() at an early, unrelated DW-shape
 * phase before ever reaching a PW phase, the already-documented "Known open
 * issues" DW_CIN_MIN_SAFE finding -- assert() halts the whole process, not
 * just that phase). mac_array_raster_integrated_wiring_tb.cpp only tests DW
 * shapes (K3S1/K7S1/K3S2/K7S2), zero PW coverage.
 *
 * Six cases, chosen to cover both the degenerate (single-chunk) and
 * chunked (multi-chunk) paths, combined with both FAST_WRITEOUT paths:
 *   1. degenerate, fast    entry3 shape (cin=48,cout=48,64x64) -- board-
 *                          validated baseline shape, well under 144KB,
 *                          1 chunk (bit-identical to the pre-chunking
 *                          build's own single PW_WEIGHT_HOIST call).
 *   2. degenerate, fast    layer_0040_pwconv shape (cin=384,cout=384,8x8)
 *                          -- w_bytes=147,456 == PW_WEIGHT_CACHE_ELEMS
 *                          EXACTLY, the boundary at which pw_n_chunks
 *                          still equals 1 (not 2) -- floor/ceil edge case.
 *   3. chunked, fast       layer_0043_pwconv shape (cin=384,cout=1152,8x8)
 *                          -- w_bytes=442,368B = exactly 3x144KB, 3 EVEN
 *                          chunks (384 ot's each). Was the direct-read
 *                          fallback case before this round.
 *   4. chunked, fast       layer_0044_pwconv shape (cin=1152,cout=384,8x8)
 *                          -- reverse direction of #3, also 3 EVEN chunks
 *                          (128 ot's each). Was the direct-read fallback
 *                          case before this round.
 *   5. degenerate, narrow  layer_0050_pwconv shape (cin=768,cout=48,1x1)
 *                          -- real narrow-path layer (w_out=1<MAC_PC),
 *                          well under 144KB, 1 chunk, unchanged semantics.
 *   6. chunked, narrow,    synthetic (cin=384,cout=960,h=8,w=1) -- matches
 *      UNEVEN last chunk  layer_0047_pwconv's real shape exactly: 3
 *                          chunks, UNEVEN (384+384+192 ot's) -- the
 *                          trickiest case, since ot_out_ch_base/ot_idx
 *                          must reflect the ABSOLUTE ot even when the
 *                          last chunk is smaller than pw_ot_per_chunk.
 *                          Also exercises pw_narrow (FAST_WRITEOUT=false)
 *                          combined with chunking for the first time.
 *============================================================*/

#include "mac_array.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

struct Lcg {
    uint32_t s;
    Lcg(uint32_t seed) : s(seed) {}
    int8_t next_i8() {
        s = s * 1664525u + 1013904223u;
        return (int8_t)((s >> 16) & 0xFF);
    }
};

static int32_t g_clip_shift(int64_t acc, int shift)
{
    int64_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return (int32_t)v;
}

static void golden_pwconv(const LayerDescV2 &d, int h_out, int w_out,
                           const std::vector<int8_t> &in, const std::vector<int8_t> &w,
                           const std::vector<int32_t> &b, std::vector<int8_t> &out)
{
    for (int co = 0; co < d.cout; co++) {
        for (int oh = 0; oh < h_out; oh++) {
            for (int ow = 0; ow < w_out; ow++) {
                int64_t acc = b[d.b_off + co];
                for (int ci = 0; ci < d.cin; ci++) {
                    int8_t x = in[d.in_off + (ci * d.h_in + oh) * d.w_in + ow];
                    int8_t wv = w[d.w_off + co * d.cin + ci];
                    acc += (int64_t)x * (int64_t)wv;
                }
                out[d.out_off + (co * h_out + oh) * w_out + ow] =
                    (int8_t)g_clip_shift(acc, d.out_shift);
            }
        }
    }
}

static bool run_case(const char *name, int cin, int cout, int h, int w, uint32_t seed)
{
    LayerDescV2 d = LayerDescV2{ LDESC_OP_PWCONV, cin, cout, h, w,
                                  /*k*/1, /*stride*/1, /*pad*/0, /*fpg*/1, /*out_shift*/7,
                                  /*in_off*/0, /*w_off*/0, /*b_off*/0, /*out_off*/cin*h*w };
    MacArrayParams p = derive_mac_array_params(d);
    d.h_out = p.h_out; d.w_out = p.w_out; d.in_ch_stride = p.in_ch_stride; d.out_ch_stride = p.out_ch_stride;
    d.n_row_tiles = p.n_row_tiles; d.n_col_tiles = p.n_col_tiles; d.n_ch_tiles = p.n_ch_tiles;
    d.last_row_tile = p.last_row_tile; d.last_col_tile = p.last_col_tile; d.last_ch_tile = p.last_ch_tile;

    const int IN_ELEMS  = cin * h * w;
    const int OUT_ELEMS = cout * d.h_out * d.w_out;
    const int TOTAL     = IN_ELEMS + OUT_ELEMS;
    const int W_ELEMS   = cout * cin;
    const int B_ELEMS   = cout;

    std::vector<int8_t>  in_i8(TOTAL, 0);
    std::vector<int8_t>  w_i8(W_ELEMS, 0);
    std::vector<int32_t> b_i32(B_ELEMS, 0);
    Lcg rng(seed);
    for (int i = 0; i < IN_ELEMS; i++) in_i8[i] = rng.next_i8();
    for (int i = 0; i < W_ELEMS; i++)  w_i8[i]  = rng.next_i8();
    for (int i = 0; i < B_ELEMS; i++)  b_i32[i] = (int32_t)rng.next_i8() * 4;

    std::vector<int8_t> gold = in_i8;
    golden_pwconv(d, d.h_out, d.w_out, in_i8, w_i8, b_i32, gold);

    std::vector<act_t> feat(TOTAL, act_t(0));
    std::vector<wt_t>  wbuf(W_ELEMS, wt_t(0));
    std::vector<acc_t> bbuf(B_ELEMS, acc_t(0));
    for (int i = 0; i < IN_ELEMS; i++) feat[i] = act_t(in_i8[i]);
    for (int i = 0; i < W_ELEMS; i++)  wbuf[i] = wt_t(w_i8[i]);
    for (int i = 0; i < B_ELEMS; i++)  bbuf[i] = acc_t(b_i32[i]);

    int written[1] = {0};
    mac_array_top(d, feat.data(), wbuf.data(), bbuf.data(), feat.data(), written,
                  reinterpret_cast<const ap_uint<32>*>(feat.data()),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())),
                  hls::burst_maxi<ap_uint<32> >(reinterpret_cast<ap_uint<32>*>(feat.data())));

    int mismatches = 0;
    for (int i = 0; i < TOTAL; i++)
        if ((int8_t)feat[i] != gold[i]) mismatches++;
    bool ok = (mismatches == 0) && (written[0] == 1);
    long w_bytes = (long)cin * cout;
    printf("[%s] cin=%d cout=%d h=%d w=%d w_bytes=%ld (%.1fKB) written=%d: %s (%d/%d mismatches)\n",
           name, cin, cout, h, w, w_bytes, w_bytes / 1024.0, written[0],
           ok ? "PASS" : "FAIL", mismatches, TOTAL);
    return ok;
}

int main()
{
    int fails = 0;
    fails += !run_case("case1_degenerate_fast_entry3",         48,   48,   64, 64, 0x11111111);
    fails += !run_case("case2_degenerate_fast_boundary_l40",   384,  384,  8,  8,  0x22222222);
    fails += !run_case("case3_chunked_fast_even_l43",          384,  1152, 8,  8,  0x33333333);
    fails += !run_case("case4_chunked_fast_even_l44",          1152, 384,  8,  8,  0x44444444);
    fails += !run_case("case5_degenerate_narrow_l50",          768,  48,   1,  1,  0x55555555);
    fails += !run_case("case6_chunked_narrow_uneven_l47shape", 384,  960,  8,  1,  0x66666666);
    printf(">>> %d/6 cases failed\n", fails);
    return fails ? 1 : 0;
}
