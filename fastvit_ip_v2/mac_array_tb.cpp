/*============================================================
 * mac_array_tb.cpp -- Phase A DW+PW PoC testbench (ZHR-63/ZHR-91).
 *
 * Two independent checks, run in one csim binary:
 *
 * PHASE 1 (correctness): call mac_array_top() for a DW layer chained into
 * a PW layer (non-8-aligned dims on purpose, to exercise the tile-
 * remainder logic), compare against a golden reference implemented HERE,
 * independently of mac_array.cpp's compute -- not by calling into it.
 * Also snapshots the weight/bias buffers before/after to prove the op
 * didn't touch anything it wasn't supposed to (the other half of ZHR-91
 * row 6's self-verified-writeback contract, alongside "output actually
 * changed").
 *
 * PHASE 2 (fault injection): re-run the same two layers, then -- AFTER
 * mac_array_top() has already set out_written[i]=1, exactly mimicking
 * the real Add defect's symptom of "IP reports done, DRAM was never
 * touched" -- overwrite one layer's output region back to its pre-call
 * contents. The self-verification step (the SAME code path used in
 * Phase 1, not a special-cased check) must then report a MISMATCH. If it
 * doesn't, the harness itself would have missed the real Add defect had
 * it existed in this architecture, and this PoC has failed its actual
 * purpose regardless of whether Phase 1 passed.
 *
 * Also dumps derive_mac_array_params()'s output for both layers to
 * mac_array_params_dump.txt, which tools/verify_mac_array_mapping.py
 * independently recomputes and diffs against (ZHR-91 row 6's second
 * requirement: descriptor-to-MAC-array-parameter mapping is NOT trusted
 * just because it compiles).
 *============================================================*/

#include "mac_array.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

/* Small deterministic LCG -- no <random> so the exact byte sequence is
 * reproducible across platforms/compilers without relying on library
 * implementation details. */
struct Lcg {
    uint32_t s;
    Lcg(uint32_t seed) : s(seed) {}
    int8_t next_i8() {
        s = s * 1664525u + 1013904223u;
        return (int8_t)((s >> 16) & 0xFF);
    }
};

/* ---- golden reference, written independently of mac_array.cpp ---- */
static int32_t g_clip_shift(int64_t acc, int shift)
{
    int64_t v = acc >> shift;
    if (v > 127)  v = 127;
    if (v < -128) v = -128;
    return (int32_t)v;
}

static void golden_dwconv(const LayerDescV2 &d, int h_out, int w_out,
                           const std::vector<int8_t> &in, const std::vector<int8_t> &w,
                           const std::vector<int32_t> &b, std::vector<int8_t> &out)
{
    for (int c = 0; c < d.cin; c++) {
        for (int oh = 0; oh < h_out; oh++) {
            for (int ow = 0; ow < w_out; ow++) {
                int64_t acc = b[d.b_off + c];
                for (int kh = 0; kh < d.k; kh++) {
                    int ih = oh * d.stride - d.pad + kh;
                    if (ih < 0 || ih >= d.h_in) continue;
                    for (int kw = 0; kw < d.k; kw++) {
                        int iw = ow * d.stride - d.pad + kw;
                        if (iw < 0 || iw >= d.w_in) continue;
                        int8_t x = in[d.in_off + (c * d.h_in + ih) * d.w_in + iw];
                        int8_t wv = w[d.w_off + (c * d.k + kh) * d.k + kw];
                        acc += (int64_t)x * (int64_t)wv;
                    }
                }
                out[d.out_off + (c * h_out + oh) * w_out + ow] =
                    (int8_t)g_clip_shift(acc, d.out_shift);
            }
        }
    }
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

/* independent, plain-int re-derivation matching mac_array.h's contract --
 * deliberately NOT calling derive_mac_array_params(), same spirit as the
 * Python cross-check but exercised here too so the dump reflects what an
 * independent implementation expects, not what the module itself claims. */
static void golden_out_dims(const LayerDescV2 &d, int &h_out, int &w_out)
{
    h_out = (d.h_in + 2 * d.pad - d.k) / d.stride + 1;
    w_out = (d.w_in + 2 * d.pad - d.k) / d.stride + 1;
}

int main()
{
    /* -- build the two-layer DW->PW chain, dims chosen to NOT be
     * multiples of 8 in any dimension, so tile-remainder handling is
     * actually exercised (20 -> 3 row/col tiles, last=4; 20 -> 3 ch
     * tiles, last=4; PW cout=13 -> 2 ch tiles, last=5). */
    LayerDescV2 desc[2];
    desc[0] = LayerDescV2{ LDESC_OP_DWCONV, /*cin*/20, /*cout*/20, /*h_in*/20, /*w_in*/20,
                           /*k*/3, /*stride*/1, /*pad*/1, /*fpg*/1, /*out_shift*/6,
                           /*in_off*/0, /*w_off*/0, /*b_off*/0, /*out_off*/8000 };
    desc[1] = LayerDescV2{ LDESC_OP_PWCONV, /*cin*/20, /*cout*/13, /*h_in*/20, /*w_in*/20,
                           /*k*/1, /*stride*/1, /*pad*/0, /*fpg*/1, /*out_shift*/7,
                           /*in_off*/8000, /*w_off*/180, /*b_off*/20, /*out_off*/16000 };

    /* Host-side step (round 3): fill in the tile-count fields a real
     * descriptor generator would compute offline, once, before the
     * hardware ever runs -- mac_array_top no longer computes any of this
     * itself (see mac_array.h). */
    for (int i = 0; i < 2; i++) {
        MacArrayParams p = derive_mac_array_params(desc[i]);
        desc[i].h_out = p.h_out;         desc[i].w_out = p.w_out;
        desc[i].n_row_tiles = p.n_row_tiles;   desc[i].n_col_tiles = p.n_col_tiles;
        desc[i].n_ch_tiles = p.n_ch_tiles;
        desc[i].last_row_tile = p.last_row_tile; desc[i].last_col_tile = p.last_col_tile;
        desc[i].last_ch_tile = p.last_ch_tile;
    }

    /* dump the host-precomputed descriptor fields for the independent
     * Python cross-check (tools/verify_mac_array_mapping.py) -- this now
     * checks "did the host compute the descriptor correctly", the same
     * contract as before, just no longer re-deriving anything hardware-side. */
    {
        FILE *f = fopen("mac_array_params_dump.txt", "w");
        for (int i = 0; i < 2; i++) {
            fprintf(f, "layer=%d op_type=%d cin=%d cout=%d h_in=%d w_in=%d k=%d stride=%d pad=%d "
                        "h_out=%d w_out=%d n_row_tiles=%d n_col_tiles=%d n_ch_tiles=%d "
                        "last_row_tile=%d last_col_tile=%d last_ch_tile=%d\n",
                    i, desc[i].op_type, desc[i].cin, desc[i].cout, desc[i].h_in, desc[i].w_in,
                    desc[i].k, desc[i].stride, desc[i].pad,
                    desc[i].h_out, desc[i].w_out, desc[i].n_row_tiles, desc[i].n_col_tiles, desc[i].n_ch_tiles,
                    desc[i].last_row_tile, desc[i].last_col_tile, desc[i].last_ch_tile);
        }
        fclose(f);
        printf("[Setup] mac_array_params_dump.txt written for tools/verify_mac_array_mapping.py "
               "(MAC_UNROLL_FACTOR=%d)\n", MAC_UNROLL_FACTOR);
    }

    const int FEAT_TOTAL = 16000 + 13 * 20 * 20;  /* A(8000) + B(8000) + C(5200) = 21200 */
    const int W_TOTAL = 20 * 3 * 3 + 13 * 20;      /* dw weight(180) + pw weight(260) = 440 */
    const int B_TOTAL = 20 + 13;                   /* dw bias(20) + pw bias(13) = 33 */

    std::vector<int8_t>  in_gold(FEAT_TOTAL, 0);
    std::vector<int8_t>  w_gold(W_TOTAL, 0);
    std::vector<int32_t> b_gold(B_TOTAL, 0);

    Lcg rng(0xC0FFEE);
    for (int i = 0; i < 8000; i++) in_gold[i] = rng.next_i8();          /* region A: DW's real input */
    for (int i = 0; i < W_TOTAL; i++) w_gold[i] = rng.next_i8();
    for (int i = 0; i < B_TOTAL; i++) b_gold[i] = (int32_t)rng.next_i8() * 4;

    int h1, w1, h2, w2;
    golden_out_dims(desc[0], h1, w1);
    golden_out_dims(desc[1], h2, w2);

    std::vector<int8_t> golden_feat = in_gold;  /* copy region A; B/C get filled below */
    golden_dwconv(desc[0], h1, w1, golden_feat, w_gold, b_gold, golden_feat);
    golden_pwconv(desc[1], h2, w2, golden_feat, w_gold, b_gold, golden_feat);

    /* ---- build HLS-typed buffers (same layout, ap_int element types) ---- */
    std::vector<act_t> feat(FEAT_TOTAL, act_t(0));
    std::vector<wt_t>  wbuf(W_TOTAL, wt_t(0));
    std::vector<acc_t> bbuf(B_TOTAL, acc_t(0));
    for (int i = 0; i < 8000; i++) feat[i] = act_t(in_gold[i]);
    for (int i = 0; i < W_TOTAL; i++) wbuf[i] = wt_t(w_gold[i]);
    for (int i = 0; i < B_TOTAL; i++) bbuf[i] = acc_t(b_gold[i]);

    /* ================= PHASE 1: correctness + no-collateral-damage ================= */
    std::vector<wt_t>  wbuf_snapshot = wbuf;
    std::vector<acc_t> bbuf_snapshot = bbuf;
    int out_written[2] = {0, 0};

    mac_array_top(desc, 2, feat.data(), wbuf.data(), bbuf.data(), feat.data(), out_written);

    bool weights_untouched = (wbuf == wbuf_snapshot) && (bbuf == bbuf_snapshot);
    bool both_written = out_written[0] == 1 && out_written[1] == 1;

    int mismatches_p1 = 0;
    for (int i = 0; i < FEAT_TOTAL; i++) {
        if ((int8_t)feat[i] != golden_feat[i]) mismatches_p1++;
    }
    bool correctness_pass = (mismatches_p1 == 0);

    printf("[Phase1] correctness: %s (%d/%d mismatches)\n",
           correctness_pass ? "PASS" : "FAIL", mismatches_p1, FEAT_TOTAL);
    printf("[Phase1] weight/bias buffers untouched by the op: %s\n",
           weights_untouched ? "PASS" : "FAIL");
    printf("[Phase1] out_written flags set for both layers: %s\n",
           both_written ? "PASS" : "FAIL");

    /* ================= PHASE 2: fault injection ================= */
    /* Re-run clean, then simulate "IP reported done, DRAM never touched"
     * for layer 1 (the PW stage) by rolling its output region back to
     * whatever was there before this call -- out_written[1] stays 1,
     * exactly like the real Add defect's op_code/ap_done/timing all
     * looking normal while the write silently didn't happen. */
    std::vector<act_t> feat2(FEAT_TOTAL, act_t(0));
    for (int i = 0; i < 8000; i++) feat2[i] = act_t(in_gold[i]);
    std::vector<act_t> pre_call_snapshot = feat2;

    int out_written2[2] = {0, 0};
    mac_array_top(desc, 2, feat2.data(), wbuf.data(), bbuf.data(), feat2.data(), out_written2);

    /* fault injection: layer 1's output region [16000, 16000+5200) reverts
     * to its pre-call contents (all zero, since region C was never
     * written before this call) -- the write silently "didn't happen". */
    for (int i = desc[1].out_off; i < FEAT_TOTAL; i++) feat2[i] = pre_call_snapshot[i];

    int mismatches_p2 = 0;
    for (int i = 0; i < FEAT_TOTAL; i++) {
        if ((int8_t)feat2[i] != golden_feat[i]) mismatches_p2++;
    }
    bool fault_was_reported_done = (out_written2[1] == 1);  /* the defect's whole danger: this stays true */
    bool selfcheck_caught_it = (mismatches_p2 > 0);          /* did independent verification notice anyway? */

    printf("\n[Phase2] fault-injected layer 1: out_written[1] still reports done: %s "
           "(this is the defect-5 symptom: IP claims success)\n",
           fault_was_reported_done ? "true (as intended)" : "false (test setup bug)");
    printf("[Phase2] self-verification (output-vs-golden compare) independently caught the "
           "silently-dropped write: %s (%d/%d mismatches in the corrupted layer)\n",
           selfcheck_caught_it ? "PASS -- CAUGHT" : "FAIL -- MISSED", mismatches_p2, FEAT_TOTAL);

    bool phase1_ok = correctness_pass && weights_untouched && both_written;
    bool phase2_ok = fault_was_reported_done && selfcheck_caught_it;

    printf("\n[Summary] Phase1 (correctness + no collateral writes): %s\n", phase1_ok ? "PASS" : "FAIL");
    printf("[Summary] Phase2 (self-verification catches a silently-dropped write): %s\n", phase2_ok ? "PASS" : "FAIL");

    return (phase1_ok && phase2_ok) ? 0 : 1;
}
