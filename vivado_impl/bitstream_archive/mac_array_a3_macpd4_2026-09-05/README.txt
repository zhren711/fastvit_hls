A3 mac_array_top bitstream -- MAC_PD 2->4 (PW channel-parallelism widening, second step)
==========================================================================================

Archived 2026-09-05. See ZHR-92 (Linear) for the full round-by-round writeup, ZHR-63 for the
mainline latency-line summary. Replaces the deployed baseline (mac_array_a3_macpd2, 2026-09-04)
as this line's new reference build -- real board and real P&R verified.

What changed
------------
Second step of the MAC_PD-widening line (first step: MAC_PD 1->2, see mac_array_a3_macpd2's own
README). MAC_PD 2->4 only -- no source change beyond the macro; the compile-time-eliminated
uncached-fallback fix from the MAC_PD=2 round already covers this (the dead branch was never
MAC_PD-specific).

The BRAM dual-port question flagged (but never reached) at MAC_PD=2 -- would pw_weight_cache need
manual cyclic partitioning to supply MAC_PD=4 simultaneous reads per cycle, given a plain BRAM only
natively offers 2 read ports -- resolved itself. pw_weight_cache remains completely unpartitioned in
source; at MAC_PD=4, HLS's own pipeline-scheduling pass auto-inferred the exact partitioning needed:

    [HLS 214-270] Inferring pragma 'array_partition type=cyclic factor=2 dim=1'
    for array '...pw_weight_cache' due to pipeline pragma

2 auto-inferred banks x 2 native BRAM read ports/bank = 4 simultaneous reads, exactly covering
MAC_PD=4's need, at HALF the naive factor (4) a hand-written partition might have reached for.
PW_FLAT achieved II=1 immediately -- zero diagnostic violation for this mechanism at all.

Isolated csynth showed a real stop-condition trigger before P&R was attempted: LUT jumped
56,358->70,661 (+25.4%) over MAC_PD=2's own isolated baseline, projecting via MAC_PD=2's own
measured real/isolated ratio (0.634) to ~84.3% real -- above every prior successful closure on this
line (77.52% was the previous high-water mark, with only +0.021ns margin). Reported as a decision
point per this round's own pre-registered "stop if LUT jumps a lot" rule. Decision: ran real P&R
anyway, on the explicit reasoning that MAC_PD=1->2's own isolated-to-real divergence went the
FAVORABLE direction and this project's history says the divergence direction is not predictable in
advance.

Real P&R (no pblock, route_design alone, no phys_opt needed)
--------------------------------------------------------------
  WNS:  +0.338785ns             (best margin on this project's ENTIRE timing history)
  LUT:  42,980/53,200 (80.79%)  (was 35,751/53,200, 67.20%, +7,229/+13.59pp -- isolated projection
                                  of ~84.3% overshot; real came in lower, but still the HIGHEST LUT
                                  occupancy this project has ever closed timing at)
  BRAM: 106/140 tiles (75.71%)  (was 82/140, 58.57%, +24 tiles)
  DSP:  50/220 (22.73%)         (was 50/220, 22.73% -- EXACTLY unchanged across MAC_PD=1/2/4)

Notable finding: a HIGHER-occupancy build (80.79% LUT) closed with a DRAMATICALLY BETTER margin
(+0.339ns) than both the MAC_PD=2 build (67.20% LUT, +0.094ns) and the MAC_PD=1 baseline (61.40%
LUT, +0.134ns) it descended from. This is a second independent data point (after the DW-raster
round's own 77.52%-at-+0.021ns vs. the gmemmeta-elim round's 59.25%-at-+0.272ns) that real timing
margin does not correlate simply with LUT occupancy percentage on this design -- recorded as a
standing lesson in CLAUDE.md. LUT% is a feasibility gate, not a timing-margin predictor.

Board verification (2026-09-05, this round)
---------------------------------------------
Single-op, byte-exact vs csim, all PASS:
  - entry3 (PW-only, degenerate/non-fallback path, cin=cout=48, 64x64): 3 repeats,
    17.30/17.36/17.30ms, mean ~17.32ms (was ~19.11ms at MAC_PD=2 -- a further -9.4%, matching the
    pre-registered ~7-8% diminishing-returns model for this step).
  - entry5_dw (DW-only): 38.88ms (was 38.82/38.87ms at MAC_PD=1/2) -- UNAFFECTED, within noise, at
    every MAC_PD value tested. Confirms the whole widening line is PW-specific; DW's own datapath
    (already `complete dim=0`-partitioned, generic in MAC_PD) never engages with any of this.

Full network (82 entries, board_test_full_network bundle):
  - 82/82 entries written, no timeouts, no hangs.
  - PL-side total: **1,522.39ms** vs. MAC_PD=2's 1,626.70ms -- -104.31ms (-6.41% further).
  - Cumulative from MAC_PD=1's own 1,806.45ms: -15.73%.
  - Cumulative from this whole latency-optimization line's original starting point (6,050ms):
    **-74.8%**.
  - Six-checkpoint correctness (primary judge, per this project's own standing process rule):
    cosine similarity vs `ckpt_ref_*_0000.npy` (the ONNX float32 reference) for stage1/stage2/
    stage3/stage4/finaldw/se: **0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 / -0.2811** -- EXACT
    match to the project's own long-established figures, at every MAC_PD value tested (1, 2, 4).
    MAC_PD widening does not change numeric semantics at any step.

Model check (this round's own pre-registered success criterion)
-------------------------------------------------------------------
MAC_PD 1->2 saved ~15.7% of PW's own time; 2->4 saved a further ~9.4% -- each doubling of
parallelism recovers roughly half of what the previous doubling did, matching the "PW's compute
share keeps halving" model exactly. MAC_PD=8 is pre-registered as very unlikely to be worth
attempting: the same pattern predicts only ~4-5% further PW-time savings, for a LUT cost that (by
the same roughly-doubling growth seen 2->4) could push real occupancy well past 90% -- a region
this project has never closed timing in. Not tested; recorded as a judgment call, not a measurement,
so a future round doesn't have to re-derive the diminishing-returns argument from scratch.

Golden rollback image /lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Source
------
fastvit_ip_v2/mac_array.h (MAC_PD 2->4; no other source change from mac_array_a3_macpd2)
