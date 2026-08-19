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
        printf("[Setup] mac_array_params_dump.txt written for tools/verify_mac_array_mapping.py\n");
    }

    bool phase0_ok = false;
    /* ================= PHASE 0: stride=2 DW correctness (round 5) =================
     * Real network coverage gap found in code review: PATCH_R_MAX/PATCH_C_MAX
     * were sized assuming stride=1 ("stride=1 assumed", literally in the old
     * comment) even though Stem and all 3 Transitions use stride=2 DW convs
     * in the real network -- at K=3/stride=2 the true receptive field is
     * 17x17=289, not the 10x10=100 the old bound allocated, an actual
     * out-of-bounds write no prior round's csim (stride=1 only) ever
     * exercised. Isolated single-layer test, own buffers, deliberately
     * non-8-aligned dims (cin=9, h_in=w_in=17) to also hit the tile-
     * remainder path at stride=2 simultaneously. */
    {
        LayerDescV2 s2 = LayerDescV2{ LDESC_OP_DWCONV, /*cin*/9, /*cout*/9, /*h_in*/17, /*w_in*/17,
                                       /*k*/3, /*stride*/2, /*pad*/1, /*fpg*/1, /*out_shift*/6,
                                       /*in_off*/0, /*w_off*/0, /*b_off*/0, /*out_off*/2601 /* 9*17*17 */ };
        MacArrayParams p = derive_mac_array_params(s2);
        s2.h_out = p.h_out; s2.w_out = p.w_out;
        s2.n_row_tiles = p.n_row_tiles; s2.n_col_tiles = p.n_col_tiles; s2.n_ch_tiles = p.n_ch_tiles;
        s2.last_row_tile = p.last_row_tile; s2.last_col_tile = p.last_col_tile; s2.last_ch_tile = p.last_ch_tile;

        const int S2_IN = 9 * 17 * 17;             /* 2601 */
        const int S2_W  = 9 * 3 * 3;                /* 81 */
        const int S2_B  = 9;
        const int S2_OUT = 9 * p.h_out * p.w_out;
        const int S2_TOTAL = S2_IN + S2_OUT;

        std::vector<int8_t>  s2_in_gold(S2_TOTAL, 0);
        std::vector<int8_t>  s2_w_gold(S2_W, 0);
        std::vector<int32_t> s2_b_gold(S2_B, 0);
        Lcg rng2(0xBADC0DE);
        for (int i = 0; i < S2_IN; i++) s2_in_gold[i] = rng2.next_i8();
        for (int i = 0; i < S2_W; i++)  s2_w_gold[i]  = rng2.next_i8();
        for (int i = 0; i < S2_B; i++)  s2_b_gold[i]  = (int32_t)rng2.next_i8() * 4;

        std::vector<int8_t> s2_golden = s2_in_gold;
        golden_dwconv(s2, p.h_out, p.w_out, s2_golden, s2_w_gold, s2_b_gold, s2_golden);

        std::vector<act_t> s2_feat(S2_TOTAL, act_t(0));
        std::vector<wt_t>  s2_wbuf(S2_W, wt_t(0));
        std::vector<acc_t> s2_bbuf(S2_B, acc_t(0));
        for (int i = 0; i < S2_IN; i++) s2_feat[i] = act_t(s2_in_gold[i]);
        for (int i = 0; i < S2_W; i++)  s2_wbuf[i] = wt_t(s2_w_gold[i]);
        for (int i = 0; i < S2_B; i++)  s2_bbuf[i] = acc_t(s2_b_gold[i]);

        int s2_written[1] = {0};
        mac_array_top(&s2, 1, s2_feat.data(), s2_wbuf.data(), s2_bbuf.data(), s2_feat.data(), s2_written);

        int mismatches_s2 = 0;
        for (int i = 0; i < S2_TOTAL; i++)
            if ((int8_t)s2_feat[i] != s2_golden[i]) mismatches_s2++;

        phase0_ok = (mismatches_s2 == 0);
        printf("[Phase0] stride=2 DW (h_in=w_in=17,cin=9 -> h_out=w_out=%d): %s (%d/%d mismatches)\n",
               p.h_out, phase0_ok ? "PASS" : "FAIL", mismatches_s2, S2_TOTAL);
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

    /* ================= PHASE 3: PW -> DW transition (round 9) =================
     * Every prior csim only ever exercised DW->PW ordering (desc[0]=DW,
     * desc[1]=PW above) -- irrelevant when the two ops were separate
     * functions with their own private acc[], but round 9 merged both onto
     * ONE shared mac_reduce_step() call site, so the untested direction
     * (PW dispatched first, DW second, same call to mac_array_top) is now
     * a real risk: does anything from PW's last REDUCE/WRITEOUT leak into
     * DW's first TAP iteration through the shared function or its gather
     * temporaries. Independent buffers, own golden check, same style as
     * Phase 0. */
    bool phase3_ok = false;
    {
        LayerDescV2 desc3[2];
        desc3[0] = LayerDescV2{ LDESC_OP_PWCONV, /*cin*/16, /*cout*/10, /*h_in*/12, /*w_in*/12,
                                 /*k*/1, /*stride*/1, /*pad*/0, /*fpg*/1, /*out_shift*/6,
                                 /*in_off*/0, /*w_off*/0, /*b_off*/0, /*out_off*/2304 };
        desc3[1] = LayerDescV2{ LDESC_OP_DWCONV, /*cin*/11, /*cout*/11, /*h_in*/15, /*w_in*/15,
                                 /*k*/3, /*stride*/1, /*pad*/1, /*fpg*/1, /*out_shift*/6,
                                 /*in_off*/3744, /*w_off*/160, /*b_off*/10, /*out_off*/6219 };
        for (int i = 0; i < 2; i++) {
            MacArrayParams p = derive_mac_array_params(desc3[i]);
            desc3[i].h_out = p.h_out; desc3[i].w_out = p.w_out;
            desc3[i].n_row_tiles = p.n_row_tiles; desc3[i].n_col_tiles = p.n_col_tiles; desc3[i].n_ch_tiles = p.n_ch_tiles;
            desc3[i].last_row_tile = p.last_row_tile; desc3[i].last_col_tile = p.last_col_tile; desc3[i].last_ch_tile = p.last_ch_tile;
        }

        const int F3_TOTAL = 8694;  /* P_in(2304)+P_out(1440)+D_in(2475)+D_out(2475) */
        const int W3_TOTAL = 160 + 99;
        const int B3_TOTAL = 10 + 11;

        std::vector<int8_t>  f3_gold(F3_TOTAL, 0);
        std::vector<int8_t>  w3_gold(W3_TOTAL, 0);
        std::vector<int32_t> b3_gold(B3_TOTAL, 0);
        Lcg rng3(0xFACE0FF);
        for (int i = 0; i < 2304; i++)             f3_gold[desc3[0].in_off + i] = rng3.next_i8();
        for (int i = 0; i < 2475; i++)             f3_gold[desc3[1].in_off + i] = rng3.next_i8();
        for (int i = 0; i < W3_TOTAL; i++)         w3_gold[i] = rng3.next_i8();
        for (int i = 0; i < B3_TOTAL; i++)         b3_gold[i] = (int32_t)rng3.next_i8() * 4;

        int h3p, w3p, h3d, w3d;
        golden_out_dims(desc3[0], h3p, w3p);
        golden_out_dims(desc3[1], h3d, w3d);
        std::vector<int8_t> f3_expected = f3_gold;
        golden_pwconv(desc3[0], h3p, w3p, f3_expected, w3_gold, b3_gold, f3_expected);
        golden_dwconv(desc3[1], h3d, w3d, f3_expected, w3_gold, b3_gold, f3_expected);

        std::vector<act_t> f3(F3_TOTAL, act_t(0));
        std::vector<wt_t>  w3buf(W3_TOTAL, wt_t(0));
        std::vector<acc_t> b3buf(B3_TOTAL, acc_t(0));
        for (int i = 0; i < F3_TOTAL; i++) f3[i] = act_t(f3_gold[i]);
        for (int i = 0; i < W3_TOTAL; i++) w3buf[i] = wt_t(w3_gold[i]);
        for (int i = 0; i < B3_TOTAL; i++) b3buf[i] = acc_t(b3_gold[i]);

        int w3ritten[2] = {0, 0};
        mac_array_top(desc3, 2, f3.data(), w3buf.data(), b3buf.data(), f3.data(), w3ritten);

        int mismatches_p3 = 0;
        for (int i = 0; i < F3_TOTAL; i++)
            if ((int8_t)f3[i] != f3_expected[i]) mismatches_p3++;
        phase3_ok = (mismatches_p3 == 0);
        printf("[Phase3] PW->DW transition (round 9 shared-array direction not covered by Phase1): "
               "%s (%d/%d mismatches)\n", phase3_ok ? "PASS" : "FAIL", mismatches_p3, F3_TOTAL);
    }

    /* ================= PHASE 4: DW -> PW(Cin=32) -> PW(Cin=8) (round 9) =================
     * Two consecutive PW calls with SHRINKING Cin (32 -> 8, both exact
     * multiples of MAC_PD so this isolates tile-count/loop-bound handling
     * across back-to-back same-op calls on the shared array from round 8's
     * zero-fill remainder handling, which Phase1/Phase3 already cover with
     * non-multiple Cin). Catches e.g. a stale n_ch_tiles/cib range from the
     * Cin=32 call leaking into the Cin=8 call now that both route through
     * the same mac_reduce_step call site. */
    bool phase4_ok = false;
    {
        LayerDescV2 desc4[3];
        desc4[0] = LayerDescV2{ LDESC_OP_DWCONV, /*cin*/9, /*cout*/9, /*h_in*/17, /*w_in*/17,
                                 /*k*/3, /*stride*/2, /*pad*/1, /*fpg*/1, /*out_shift*/6,
                                 /*in_off*/0, /*w_off*/0, /*b_off*/0, /*out_off*/2601 };
        desc4[1] = LayerDescV2{ LDESC_OP_PWCONV, /*cin*/32, /*cout*/6, /*h_in*/10, /*w_in*/10,
                                 /*k*/1, /*stride*/1, /*pad*/0, /*fpg*/1, /*out_shift*/6,
                                 /*in_off*/3330, /*w_off*/81, /*b_off*/9, /*out_off*/6530 };
        desc4[2] = LayerDescV2{ LDESC_OP_PWCONV, /*cin*/8, /*cout*/5, /*h_in*/9, /*w_in*/9,
                                 /*k*/1, /*stride*/1, /*pad*/0, /*fpg*/1, /*out_shift*/6,
                                 /*in_off*/7130, /*w_off*/273, /*b_off*/15, /*out_off*/7778 };
        for (int i = 0; i < 3; i++) {
            MacArrayParams p = derive_mac_array_params(desc4[i]);
            desc4[i].h_out = p.h_out; desc4[i].w_out = p.w_out;
            desc4[i].n_row_tiles = p.n_row_tiles; desc4[i].n_col_tiles = p.n_col_tiles; desc4[i].n_ch_tiles = p.n_ch_tiles;
            desc4[i].last_row_tile = p.last_row_tile; desc4[i].last_col_tile = p.last_col_tile; desc4[i].last_ch_tile = p.last_ch_tile;
        }

        const int F4_TOTAL = 8183;  /* D_in(2601)+D_out(729)+P32_in(3200)+P32_out(600)+P8_in(648)+P8_out(405) */
        const int W4_TOTAL = 81 + 192 + 40;
        const int B4_TOTAL = 9 + 6 + 5;

        std::vector<int8_t>  f4_gold(F4_TOTAL, 0);
        std::vector<int8_t>  w4_gold(W4_TOTAL, 0);
        std::vector<int32_t> b4_gold(B4_TOTAL, 0);
        Lcg rng4(0xDEADFA11);
        for (int i = 0; i < 2601; i++)      f4_gold[desc4[0].in_off + i] = rng4.next_i8();
        for (int i = 0; i < 3200; i++)      f4_gold[desc4[1].in_off + i] = rng4.next_i8();
        for (int i = 0; i < 648; i++)       f4_gold[desc4[2].in_off + i] = rng4.next_i8();
        for (int i = 0; i < W4_TOTAL; i++)  w4_gold[i] = rng4.next_i8();
        for (int i = 0; i < B4_TOTAL; i++)  b4_gold[i] = (int32_t)rng4.next_i8() * 4;

        int h4d, w4d, h4p32, w4p32, h4p8, w4p8;
        golden_out_dims(desc4[0], h4d, w4d);
        golden_out_dims(desc4[1], h4p32, w4p32);
        golden_out_dims(desc4[2], h4p8, w4p8);
        std::vector<int8_t> f4_expected = f4_gold;
        golden_dwconv(desc4[0], h4d, w4d, f4_expected, w4_gold, b4_gold, f4_expected);
        golden_pwconv(desc4[1], h4p32, w4p32, f4_expected, w4_gold, b4_gold, f4_expected);
        golden_pwconv(desc4[2], h4p8, w4p8, f4_expected, w4_gold, b4_gold, f4_expected);

        std::vector<act_t> f4(F4_TOTAL, act_t(0));
        std::vector<wt_t>  w4buf(W4_TOTAL, wt_t(0));
        std::vector<acc_t> b4buf(B4_TOTAL, acc_t(0));
        for (int i = 0; i < F4_TOTAL; i++) f4[i] = act_t(f4_gold[i]);
        for (int i = 0; i < W4_TOTAL; i++) w4buf[i] = wt_t(w4_gold[i]);
        for (int i = 0; i < B4_TOTAL; i++) b4buf[i] = acc_t(b4_gold[i]);

        int w4ritten[3] = {0, 0, 0};
        mac_array_top(desc4, 3, f4.data(), w4buf.data(), b4buf.data(), f4.data(), w4ritten);

        int mismatches_p4 = 0;
        for (int i = 0; i < F4_TOTAL; i++)
            if ((int8_t)f4[i] != f4_expected[i]) mismatches_p4++;
        phase4_ok = (mismatches_p4 == 0);
        printf("[Phase4] DW(stride=2)->PW(Cin=32)->PW(Cin=8) mixed sequence: "
               "%s (%d/%d mismatches)\n", phase4_ok ? "PASS" : "FAIL", mismatches_p4, F4_TOTAL);
    }

    printf("\n[Summary] Phase0 (stride=2 DW correctness): %s\n", phase0_ok ? "PASS" : "FAIL");
    printf("[Summary] Phase1 (correctness + no collateral writes): %s\n", phase1_ok ? "PASS" : "FAIL");
    printf("[Summary] Phase2 (self-verification catches a silently-dropped write): %s\n", phase2_ok ? "PASS" : "FAIL");
    printf("[Summary] Phase3 (PW->DW transition on the shared array): %s\n", phase3_ok ? "PASS" : "FAIL");
    printf("[Summary] Phase4 (DW->PW->PW mixed sequence, shrinking Cin): %s\n", phase4_ok ? "PASS" : "FAIL");

    return (phase0_ok && phase1_ok && phase2_ok && phase3_ok && phase4_ok) ? 0 : 1;
}
