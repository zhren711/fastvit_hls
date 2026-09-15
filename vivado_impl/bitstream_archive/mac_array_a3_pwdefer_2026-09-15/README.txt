mac_array_a3_pwdefer -- deployed baseline as of 2026-09-15 (ZHR-92)
====================================================================

Supersedes mac_array_a3_rowread (2026-09-14: ~679ms, PW 495.1ms, WNS +0.113ns).

What changed: PW_DEFER_WRESP (commit fa44bd0 + default flip this round).
PW_FLAT's FAST_WRITEOUT issued write_request + write + write_response for
every 4-byte output word, all three inside ONE II=1 iteration (schedule:
writereq ST_9, write ST_10, 5-stage writeresp ST_11-15 -- the identical
stage numbers DW's CROW_CCOL had before DWR_ROWBURST), so every word waited
for its own B response inside the pipeline: 907,056 words x 18 cycles =
~163ms of PW's 495ms (real-board fit, R^2 0.992). One tile's 4 rows are
w_out apart, so DW's row burst does not apply -- but the stall is the
RESPONSE wait, not the request count. Now: request + write stay exactly
where they were (wr_col==MAC_PC-1); write_response() is popped at writeout
ROW STARTS (wr_col==0) only once >= 8 are pending, i.e. the ot two back's
responses are collected during this ot's writeout rows (gap >=
2*(n_cbase*8+16)-4 >= 60 cycles for every real cin); <= 8 in flight vs the
gmem_act adapter's NUM_WRITE_OUTSTANDING = USER_MAXREQS = 16 (read from the
exported RTL); the <= 8 left after the loop are drained by PW_WRESP_DRAIN.
Step-1 schedule probe before implementing: writereq (ST_5) / write (ST_10)
on predicate (wr_col==3), writeresp (ST_4-8) on the DISJOINT predicate
(wr_col==0 & pending>=8) -- request and response in different iterations
of the SAME loop, the data write never left the pipeline (not the
PW_WRITEOUT_FLUSH shape, 2026-09-04, +142%). PW_DEFER_WRESP_OFF reverts.

Files
-----
  mac_array_bd_wrapper_pwdefer.bit           raw, md5 b5b5e735c9a34f34abceb2d0839a8759
  mac_array_bd_wrapper_pwdefer_swapped.bin   board-pulled /lib/firmware/pwdefer.bin, md5 443f787976768284f1b2e948c1137a7d
  timing_wide_pwdefer.rpt, utilization_pwdefer.rpt

Deployed on the board as /lib/firmware/pwdefer.bin (raw .bit at
/home/root/fpga/pwdefer.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin
NOT touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.291791ns   (rowread +0.113 -- placement roll on the route-dominated
                       population, see the timing report)
  LUT:  43,909/53,200 (82.54%)   (rowread 44,422: -513; isolated said +480 --
                                  opposite sign, another data point)
  BRAM: 107/140 (76.43%)         (flat)
  DSP:  32/220 (14.55%)          (flat)

csim (SIX suites, the PW pair re-paired this round -- both had been on the
9-arg pre-ELEMWISE_BURST signature since 2026-09-06 and were the ONLY PW
coverage): raster 5/5, wiring 4/4, gelu_add 8/8, scalar_ops 4/4,
pw_weight_hoist 6/6, pw_scaling_probe 20/20 -- with the flag and flag-less.

Board (2026-09-15), pre-registered order, 30s timeouts
------------------------------------------------------
1. entry3 (PW-only) FIRST, 3 runs: byte-exact 0/196608, 17.1 -> 7.45 / 7.52 / 7.53ms
   (-56%). Chunked/boundary PW entries 60/64/66/70/72 all byte-exact (the big-cin
   ones gain little: they are ROW_READ-request dominated, as the fit says).
2. Controls entry5_dw 0/196608 (5.45ms, flat), SE ops 75/77/79/80 byte-exact,
   entry0 GELU 0/786432, entry10 ADD 0/196608.
3. Full network 82/82, two runs: PL total 501.70 / 491.45ms (rowread ~679: -27%;
   pre-registered 540-570 -> below it); per-entry sums 470.2 / 471.8; all 7
   checkpoint files MD5-identical. PWCONV 305.71 / 307.46ms (-38%; pre-registered
   350-380 = "mechanism works" -> below it); DWCONV 115.5 (flat), GELU 22.9,
   ADD 13.0, SE 12.9 (flat). ONNX cosine EXACT 0.6313/0.1286/0.2227/0.3491/
   -0.2459/-0.2811.

Refit on this run (R^2 0.994): PW 307ms = 1.12 cycles per PW_FLAT iteration
(147ms; floor 131) + ~0 per output word (-0.11 -- the 163ms term is GONE) +
71 cycles per ROW_READ request (128ms) + 1.05 cycles per weight byte (30ms).
Next lever: ROW_READ requests (128-153ms, ~45% of PW) -- 71-85 cycles per
request for only w/4 (2-16) words: issue the 4 rr requests (or a ci-group's
16) back-to-back before the fill loops so the latencies overlap.

Cumulative on this latency line: 6,050ms -> ~496ms = -91.8%.
Operator split now: PW 62%, DW 23%, GELU 4.6%, ADD 2.6%, SE 2.6%.

Register map: UNCHANGED (hw.h identical). *_sohoist board binaries remain valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_pwdefer.tcl (built with -DPW_DEFER_WRESP;
   since the default flip the flag-less build is the same design -- verified)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_pwdefer.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
