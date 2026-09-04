A3 mac_array_top bitstream -- MAC_PD 1->2 (PW channel-parallelism widening, dead-branch fix)
==============================================================================================

Archived 2026-09-04. See ZHR-92 (Linear) for the full round-by-round writeup, ZHR-63 for the
mainline latency-line summary. Replaces the deployed baseline (mac_array_a3_pw_wchunk, 2026-09-03)
as this line's new reference build -- real board and real P&R verified.

What changed
------------
MAC_PD-expansion (channel-parallelism widening) was CLOSED on 2026-08-31: PW_FLAT's achieved II
regressed 1->2 at MAC_PD=2 due to gmem_w's single AXI port being unable to service more than one
bus request per pipeline iteration, exactly canceling the halved iteration count on real hardware
(measured 3.24% SLOWER at the time).

That closure's own premise no longer holds on the current architecture: weight caching
(pw_weight_cache/PW_WCHUNK, deployed 2026-09-02/03) means PW_FLAT no longer reads gmem_w on its real
hot path -- every real PW layer is served from on-chip BRAM. Re-checking MAC_PD=2 against the current
source still showed achieved II=2, and the diagnostic still named gmem_w -- but a follow-up test
(forcing the uncached fallback arm to compile-time-dead via `if(true)`) recovered II=1 immediately.
Root cause: the direct-DRAM-read fallback arm inside pw_flat_pipeline_impl is unreachable on every
real PW layer (pw_cached_ok = d.cin <= PW_WEIGHT_CACHE_ELEMS, always true for real cin<=1152 vs
cache=147,456) but was still a runtime `if(pw_cached)` branch, not a template. At MAC_PD=1 this was
safe (the original design's own reasoning: "only one arm touches gmem_w" -- true when the per-`dd`
loop had exactly 1 iteration). At MAC_PD=2 the unrolled `dd` loop duplicates the WHOLE if/else across
2 simultaneous lanes, and HLS conservatively schedules for both lanes hitting the dead gmem_w arm at
once -- a scheduler artifact from dead code, not real bandwidth demand.

Fix: the dead uncached arm is now compile-time-eliminated by default (`if(true)` instead of
`if(pw_cached)`). The old runtime-gated behavior is preserved, unused, behind a new
PW_ALLOW_UNCACHED_FALLBACK opt-in flag, per this project's own dead-but-kept-fallback convention
(use_wide_path, PW_PATCH_HOIST's in_base_wide parameter).

Real P&R (no pblock, route_design alone, no phys_opt needed)
--------------------------------------------------------------
  WNS:  +0.094463ns             (was +0.134ns -- thinner margin, still positive/closed)
  LUT:  35,751/53,200 (67.20%)  (was 32,664/53,200, 61.40%, +3,087/+5.80pp)
  BRAM: 82/140 tiles (58.57%)   (was 74/140, 52.86%, +8 tiles)
  DSP:  50/220 (22.73%)         (was 50/220, 22.73% -- exactly unchanged)

Real P&R closed on the FIRST attempt -- no shared-multiplier-style regression chase needed, unlike
PW_WCHUNK's own 3-round saga on the same general class of change.

Board verification (2026-09-04, this round)
---------------------------------------------
Single-op, byte-exact vs csim, all PASS:
  - entry3 (PW-only, degenerate/non-fallback path, cin=cout=48, 64x64): 22.69ms -> 18.41-19.46ms
    across 3 repeats, mean ~19.11ms (-14.1% to -18.7%, mean ~15.7%).
  - entry5_dw (DW-only): 38.82ms -> 38.87ms, UNAFFECTED (within noise) -- confirms the win is
    specific to PW's gmem_w-adjacent mechanism; DW's own datapath (already `complete dim=0`-
    partitioned in dw_patch/dw_wtile/acc, generic in MAC_PD) neither gains nor regresses.

Full network (82 entries, board_test_full_network bundle):
  - 82/82 entries written, no timeouts, no hangs.
  - PL-side total: **1,626.70ms** vs. baseline's 1,806.45ms -- -179.75ms (-9.95%).
  - Cumulative from this whole latency-optimization line's original starting point (6,050ms):
    **-73.1%**.
  - Six-checkpoint correctness (primary judge, per this project's own standing process rule):
    cosine similarity vs `ckpt_ref_*_0000.npy` (the ONNX float32 reference) for stage1/stage2/
    stage3/stage4/finaldw/se: **0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 / -0.2811** -- EXACT
    match to the project's own long-established figures (4 decimal places). MAC_PD=2 does not
    change numeric semantics.

Model check (this round's own pre-registered success criterion)
-------------------------------------------------------------------
PW's compute-time share of real board time was previously measured at 28% (cosim decomposition,
B1 shape). Doubling PW_FLAT's channel parallelism halves that 28% compute share, predicting ~14%
of PW's own time saved (not 2x, since 72% of PW's time is non-compute). Measured PW-only reduction
(~15.7%, entry3) landed close to this prediction once properly scoped to a PW-only shape -- the
model held. The full-network aggregate (-9.95%) sits between PW-only's -15.7% and DW's ~0% because
the network mixes both op types; implies PW is roughly 63% of full-network time by this arithmetic,
not evidence of serialization eating into the model's prediction.

Not reached this round: whether MAC_PD=4 is viable. The BRAM dual-port question (pw_weight_cache is
a plain, unpartitioned array with 2 native read ports -- matches MAC_PD=2 exactly, insufficient for
MAC_PD=4 without partitioning) was flagged at the start but never actually reached, since gmem_w's
dead-branch artifact was the binding constraint at MAC_PD=2, not BRAM ports. csim was not separately
re-run this round (only isolated csynth + real board); real hardware correctness (byte-exact
single-op + exact ONNX cosine match) was treated as sufficient given scope, but a formal csim pass
is still owed if this build is revisited. 150MHz was deliberately deferred to its own separate round
to avoid confounding two simultaneous architectural changes.

Golden rollback image /lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Source
------
fastvit_ip_v2/mac_array.h (MAC_PD 1->2)
fastvit_ip_v2/mac_array_raster_integrated.cpp (pw_flat_pipeline_impl's pw_cached branch --
  compile-time-eliminated uncached arm by default, PW_ALLOW_UNCACHED_FALLBACK opt-in to restore
  the old runtime-gated behavior)
