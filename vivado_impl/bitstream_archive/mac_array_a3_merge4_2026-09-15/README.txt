mac_array_a3_merge4 -- deployed baseline as of 2026-09-15 (ZHR-92)
===================================================================

Supersedes mac_array_a3_pwpf (same day: ~418ms, PW 231ms, WNS +0.131ns).

What changed: PW_ROWREAD_MERGE4 (commit 325d083 + default flip this round) --
(a)+(b) on ROW_READ. After PW_ROWREAD_PREFETCH the fit still had 34 cycles per
ROW_READ request (61ms), uniform across layers and independent of cin -- not
AXI latency but loop overhead (ROW_READ_FILL ran its compile-time
MAX_WORDS_PER_CH = 17 iterations regardless of n_words, plus ~17 cycles of
fill/drain, request issue and loop control per request).
(b) ONE request per (rt, ci) covering the row tile's 4 input rows -- contiguity
verified first: all 26 PW layers have in_ch_stride == h_in*w_in (packed rows),
ROW_READ reads whole rows (k=1/s=1), the tile's rows are oh = rt*4+rr, so the
4 rows are one contiguous 4*W-byte span (unlike a DW output tile). Requests
179,904 -> 44,976. (a) the FILL bound is the runtime span word count -- (b)
needs (a): a fixed bound would be 65, worse than today for small W.
Three formulations to reach FILL II=1: per-lane chained (row,col) counters and
per-word counters with a one-row spill both got II=4 (store-vs-store 200-880:
data-dependent lane addresses, HLS cannot prove distinct banks); what worked is
building the store index as (word << 2) | lane so the bank IS the lane, plus a
per-LAYER guard (W % 4 == 0 && in_off % 4 == 0 -- every real conv PW layer)
outside the loops; the W=1 SE fc layers and synthetic W<4 shapes keep the
original per-(rr,ci) path, which stays compiled in (isolated LUT +2,106; real
+360). Prefetch depth scales with the span (W>32 -> 2, W>16 -> 4, W>8 -> 6,
else 8) to stay inside the adapter's 256-word read buffer and 16 outstanding
bursts. No new multiply (binding DB). PW_ROWREAD_MERGE4_OFF reverts.

Files
-----
  mac_array_bd_wrapper_merge4.bit           raw, md5 95b1f9561e5e15488525d4adc5c77da3
  mac_array_bd_wrapper_merge4_swapped.bin   board-pulled /lib/firmware/merge4.bin, md5 98a1e49bb88442262f393986e9a03f58
  timing_wide_merge4.rpt, utilization_merge4.rpt

Deployed on the board as /lib/firmware/merge4.bin (raw .bit at
/home/root/fpga/merge4.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.460899ns   -- the best on this line's whole history (previous best
                         macpd4's +0.339); the +2,106 isolated LUT did not become
                         a resource/timing problem
  LUT:  44,590/53,200 (83.82%)   (pwpf 44,230: +360; isolated said +2,106, ~17%)
  BRAM: 107/140, DSP 32/220     (flat)

csim: six suites, with the flag and flag-less: raster 5/5, wiring 4/4,
gelu_add 8/8, scalar_ops 4/4, pw_weight_hoist 6/6, pw_scaling_probe 20/20.

Board (2026-09-15), pre-registered order, 30s timeouts
------------------------------------------------------
1. entry3 (W=64) x3: byte-exact, 5.9 -> 5.73 / 5.62 / 5.32ms (-5% -- the LEAST,
   as pre-registered). W=8 chunked entries: entry66 17.5 -> 11.7 (-33%), entry72
   15.3 -> 9.7 (-36%), entry64 12.9 -> 10.3 (-20%), entry70 11.7 -> 9.0 (-23%),
   all byte-exact -- the MOST, as pre-registered (119 - 4*n_words cycles saved
   per (rt,ci) group; W=8 groups save 111, W=64 groups 55).
2. Controls entry5_dw / SE ops / entry0 GELU / entry10 ADD byte-exact, flat.
3. Full network 82/82, two runs: PL total 378.16 / 377.60ms (pwpf ~418: -9.5%;
   pre-registered 370-390); per-entry sums 357.9 / 357.8; all 7 checkpoint
   files MD5-identical. PWCONV 193.45 / 193.38ms (-16.3%; pre-registered
   185-200); DWCONV 115.6, GELU 22.8, ADD 13.0, SE 13.0 (flat). ONNX cosine
   EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.
   Per-W check against the model (pwpf -> merge4, model in brackets):
   W=8 -17.2ms [-20.0], W=16 -12.9 [-13.1], W=32 -4.3 [-5.7], W=64 -3.4 [-3.8],
   W=1 0.0 [general path] -- ordering and magnitudes as predicted.

Refit on this run (R^2 0.964): PW 193ms = 1.11 cycles/PW_FLAT iteration
(146ms; floor 131) + 50 cycles per (rt,ci) request (22ms; 44,976 requests) +
0.72 cycles/weight byte (21ms). Compute is at 1.11x its floor, weights at
floor, requests ~22ms: PW is within ~15% of its floor on every term.

Cumulative on this latency line: 6,050ms -> ~378ms = -93.8%.
Operator split now: PW 51%, DW 31%, GELU 6.0%, ADD 3.5%, SE 3.4%.

Register map: UNCHANGED (hw.h identical). *_sohoist board binaries remain valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_merge4.tcl (built with -DPW_ROWREAD_MERGE4;
   since the default flip the flag-less build is the same design -- verified)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_merge4.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
