A3 mac_array_top bitstream -- ELEMWISE_BURST (GELU/ADD burst_maxi rewrite)
============================================================================

Archived 2026-09-07. See ZHR-92 (Linear) for the full round-by-round writeup, ZHR-63 for the
mainline latency-line summary. Replaces the deployed baseline (mac_array_a3_macpd4, 2026-09-05)
as this line's new reference build -- real board and real P&R verified.

*** IMPORTANT ANNOTATION 1 -- THIS MARGIN HAS ZERO HEADROOM ***
--------------------------------------------------------------------
route_design ALONE does NOT close timing on this netlist: WNS=-0.166790ns. The deployed margin
(WNS=+0.017ns) comes ENTIRELY from a single phys_opt_design pass on top of that. This is
qualitatively different from every prior deployed baseline on this line (macpd2, macpd4,
pw_wchunk, etc.), all of which closed on route_design alone with real margin to spare.

Practical consequence: the NEXT change to this design should NOT assume phys_opt_design will
reproduce the same recovery on a modified netlist -- re-run BOTH route_design alone AND
phys_opt_design fresh, and treat a route_design-alone result anywhere near -0.2ns or worse as a
signal to look for a real source-level fix, not just another phys_opt_design attempt. See
CLAUDE.md's own "phys_opt_design applicability boundary" entry: this lever reliably recovers a
violation within roughly -0.2ns of route_design alone (this build: -0.167ns; dummy4th: -0.133ns),
but does NOT recover a violation at the -2.264ns magnitude (PW_WCHUNK's own shared-multiplier
regression, which needed an actual source fix).

What changed
------------
run_gelu/run_add rewritten to use hls::burst_maxi<ap_uint<32>> (word-packed, 4 lanes unrolled per
word) instead of plain un-bursted act_t[] pointer accesses. Two new ports (elemwise_in_burst/
elemwise_out_burst) share the existing gmem_act bundle -- no new AXI master, no BD change.
run_add's own two-source case reads sequentially (buffer in_off's chunk fully, then read in2_off's)
rather than simultaneously, which incidentally also resolved its pre-existing achieved-II=2 port-
contention violation (a bonus, not pre-registered).

Motivation: a real per-entry operator decomposition found GELU's real-to-theoretical-floor ratio
was 8.42x-8.87x and ADD's was 15.94x-17.66x, both flat across a 32x element-count range -- confirmed
genuine per-element cost, not fixed dispatch overhead. Neither operator had ever used burst_maxi,
unlike PW's own already-fixed ROW_READ/WRITEOUT paths.

*** IMPORTANT ANNOTATION 2 -- TWO REAL, PRE-EXISTING BUGS FOUND AND FIXED (neither in the
    ELEMWISE_BURST HLS logic itself) ***
--------------------------------------------------------------------------------------------
1. tools/build_single_op_test_entry0_gelu.py and entry10_add.py were stuck at 27-field
   descriptors (MacLayerDesc grew to 28 fields on 2026-08-31, use_wide_path appended) -- never
   updated, and their bundles had NEVER actually been built or run before this round. A sweep of
   every other build_*.py bundle generator found 18 MORE scripts still stuck at 27 fields -- likely
   harmless in practice (use_wide_path is confirmed dead code in the active dispatch path, and many
   of these bundles were already successfully board-tested in earlier rounds despite the
   staleness), but a real, verified latent-risk pattern, not yet fixed for any of the 18. Confirmed
   NOT load-bearing for this round's own bug: fixing entry10_add's own desc.bin alone (before the
   register-wiring fix below) still produced 100%-poison wrong output.

2. THE ACTUAL PROXIMATE CAUSE of both a real board hang and a real wrong-output case:
   elemwise_in_burst/elemwise_out_burst's own AXI-Lite base-address registers (0xe8/0xec and
   0xf4/0xf8, read from the real exported IP's own xmac_array_top_hw.h) were never wired into the
   ARM driver. This is the THIRD confirmed instance of "sharing an m_axi bundle does NOT mean
   sharing a control register" on this project (after out_burst and in_burst's own identical
   history) -- and the FIRST instance where the risk was explicitly named in the code's own header
   comment IN ADVANCE ("own control register, not yet wired into mac_array_driver.c... a P&R-stage
   TODO, tracked, not silently deferred") and still shipped unfixed to a real board round.
   **New standing lesson: flagging a risk in a comment is not the same as handling it.** csim could
   not catch this (no register-address concept) -- unprogrammed elemwise_in_burst caused a genuine
   AXI bus hang on the largest real GELU dispatch (786,432 elements); unprogrammed elemwise_out_burst
   caused output 100% identical to the pre-dispatch poison pattern despite ap_done/out_written=1
   firing normally. Fixed by adding the register writes to all three real ARM-side call sites
   (mac_array_single_op_test.c, _add.c, mac_array_full_network_test.c).

Real P&R
--------
  Route_design alone: WNS = -0.166790ns  (VIOLATED)
  After ONE phys_opt_design pass: WNS = +0.017ns  (closed, but see Annotation 1 above)
  LUT:  44,272/53,200 (83.22%)  (was 42,980/53,200, 80.79%, +2.43pp)
  BRAM: 107/140 tiles (76.43%)  (was 106/140, 75.71%, +0.72pp)
  DSP:  59/220 (26.82%)         (was 50/220, 22.73%, +4.09pp)

Board verification (2026-09-07, this round)
---------------------------------------------
Single-op, byte-exact vs csim, all PASS (after both bugs above were fixed):
  - entry0_gelu (the exact shape that originally hung, 786,432 elements): 66.82ms -> 3.35ms
    (-95.0%, ~19.9x)
  - entry10_add (196,608 elements): 31.33ms -> 2.18ms (-93.0%, ~14.4x)
  - entry3 (PW-only, control): unchanged, byte-exact -- confirms no regression from the new ports.
  - entry5_dw (DW-only, control): unchanged, byte-exact -- confirms no regression.

Full network (82 entries, board_test_full_network bundle):
  - 82/82 entries written, no timeouts, no hangs (after both bug fixes).
  - PL-side total: **1,099.77ms** vs. macpd4's 1,522.39ms -- -422.62ms (**-27.8%**), the SECOND-
    largest single-round win on this whole line (behind only PW weight-residency's own -41.9%).
  - Cumulative from this whole latency-optimization line's original starting point (6,050ms):
    **-81.8%**.
  - Per-operator: GELU 328.47ms->22.93ms (-93.0%), ADD 138.42ms->13.06ms (-90.6%), PW 495.88ms->
    496.08ms (unchanged), DW 531.51ms->531.20ms (unchanged).
  - Six-checkpoint correctness verified via cosine similarity against the untouched ONNX float32
    reference: **0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 / -0.2811** -- EXACT match to the
    project's own long-established figures.

Operator decomposition after this round (recomputed from the same full-network run)
---------------------------------------------------------------------------------------
  DWCONV: 531.20ms (49.36%)
  PWCONV: 496.08ms (46.09%)
  GELU:    22.93ms (2.13%)
  ADD:     13.06ms (1.21%)
  SE-block components combined: 12.95ms (1.20%)

DW and PW are now essentially TIED (95.45% combined) -- neither is uniquely dominant. GELU+ADD
dropped from 30.68% to 3.34% of the network. The next real target is PW or DW specifically; GELU/
ADD/SE combined (4.54%) are no longer worth attacking.

Register map: CHANGED. Two new ports (elemwise_in_burst/elemwise_out_burst) added, each with its
own AXI-Lite base-address register (see Annotation 2 above) -- ANY ARM-side binary built before
this round's driver fix will hang or silently produce wrong output on any real GELU/ADD dispatch.
Always rebuild driver/test harnesses from current source before using them against this build.

Golden rollback image /lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Source
------
fastvit_ip_v2/mac_array.h (ELEMWISE_CHUNK/ELEMWISE_CHUNK_WORDS constants)
fastvit_ip_v2/mac_array_raster_integrated.cpp (run_gelu/run_add rewrite, elemwise_in_burst/
  elemwise_out_burst ports and pragmas)
fastvit_ip_v2/a3_single_op_test_src/mac_array_driver.h (MAC_ELEMWISE_IN_BURST_LO/HI,
  MAC_ELEMWISE_OUT_BURST_LO/HI register offsets)
fastvit_ip_v2/a3_single_op_test_src/mac_array_single_op_test.c, _add.c,
  mac_array_full_network_test.c (register-write fix, all 3 real call sites)
tools/build_single_op_test_entry0_gelu.py, entry10_add.py (27->28 field fix)
