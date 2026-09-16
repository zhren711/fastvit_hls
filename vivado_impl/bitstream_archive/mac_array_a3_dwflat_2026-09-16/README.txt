mac_array_a3_dwflat -- deployed baseline as of 2026-09-16 (ZHR-92)
===================================================================

Supersedes mac_array_a3_worow (2026-09-15: ~229ms / DW 67.8 / PW 136.4 / WNS
+0.285ns). All numbers on the busy-poll, deferred-I/O ARM harness (*_whoist
binaries; register map unchanged).

What changed: DWR_FLAT (commit 9023d7d + the counter-narrowing fix + default
flip this round; DWR_FLAT_OFF reverts). The DW iteration-structure
decomposition (first done this round, PW-style): 3,552,000 beats = 35.5ms at
II=1 (55.7% valid outputs), real MACs 59.1M on the 98-tap array (algorithmic
floor 6.0ms, measured 67.8 = 11.2x; the 12 k=3 layers use 9 of 98 taps). 67.8
= 35.5 (beats) + 22.8 (per-row glue: CROW sequential around a request loop,
CCOL fill/drain, L1_DRAIN, a response loop -- ~23.5 cycles x 97,344 rows) +
10.1 (per-channel prologue). This round removes most of the 22.8: dwr_produce's
ROW x COL and dwr_consume's CROW x CCOL are each ONE II=1 pipelined loop per
channel (total_beats = h_pad*w_pad, narrow-typed, once per layer). Row state is
recomputed at pcol==0; the row-boundary bus ops sit on iterations that provably
carry no lane-0 data write (the first lane-0 write of a row is at pcol =
K-1+3S >= 5, and the iteration after any lane-0 write is free): pcol 0 lane-1
write_request (fpg=2, for the buffered row), pcol 1 lane-0 write_request,
pcol 2..n+1 lane-1 drain words, one write_response pop after each lane-0 write
while > 3 rows' worth are pending (<= 8 in flight). fpg=2 AXI order (AW
lane1(R) before lane0(this) -> data lane1(R) then lane0(this)) requires a
lane-1 row to drain only after its successor valid row finished: a 2-deep ring,
csim-asserted n+1 < K-1+3S (real: 9<12, 3<5). Two bus-write call sites in one
body were a 200-880 (II=2): merged into ONE write with a muxed source.
Produce: the one-row-ahead read_request at pcol==0.

THE P&R LESSON OF THIS ROUND (now a CLAUDE.md rule): the first P&R came back at
WNS -2.574ns although step 1 showed II=1 and zero violations -- the csynth log
had already said the consume loop estimated 14.986ns and the produce loop
10.566ns (HLS 200-871/200-1016): 32-bit int loop-carried counters (w_pend
updated at three sites, col_phase/row_phase reset+increment+wrap, wbuf_n
++/==4/mux) chained inside the II=1 bodies. Narrowing each to its real width
(ap_uint<3>/<5>/<8>) and folding multi-site updates into one net add took the
estimates to 8.20 / 7.30ns and the real P&R to +0.135 with NO other change.

Files
-----
  mac_array_bd_wrapper_dwflat.bit           raw, md5 a77f655be0c6a7d6f028fc3aea110cda
  mac_array_bd_wrapper_dwflat_swapped.bin   board-pulled /lib/firmware/flat.bin, md5 caa5b1be495fb52c698a16f8f6ce176f
  timing_wide_dwflat.rpt, utilization_dwflat.rpt

Deployed on the board as /lib/firmware/flat.bin (raw .bit at
/home/root/fpga/flat.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.135054ns   (worow +0.285; worst path desc_op_type register ->
                       gmem_act load-unit read buffer enable, 82% route --
                       the route-dominated population)
  LUT:  45,614/53,200 (85.74%)   (worow 46,144: -530; isolated said +801 --
                                  the row-nested FSMs cost more than the flat ones)
  BRAM: 111/140 (+1: the lane-1 ring), DSP 28/220 (+2)

csim: with the flag raster 5/5 (both fpg=2 layers and the stride-2 layer),
wiring 4/4, gelu_add 8/8, scalar_ops 4/4, pw_weight_hoist 6/6,
pw_scaling_probe 20/20; flag-less after the default flip: see the promotion
commit (csynth totals bit-identical 255/22/40,327/79,843, hw.h identical,
six suites clean).

Board (2026-09-16, *_whoist binaries md5-verified; pre-registered order)
------------------------------------------------------------------------
1. entry5_dw x3 (k3, 66 rows/ch): 2.924 -> 2.229 / 2.232 / 2.230ms (-24%;
   pre-registered ~2.0); byte-exact.
2. Controls byte-exact and flat: entry3 3.782, entry66 7.351, GELU 2.675,
   ADD 1.188, GAP 0.775, SCALE 0.176.
3. Full network 82/82, two runs: 215.14 / 215.14ms (worow 229: -6.1%;
   pre-registered 208-211); per-entry sums 209.42 / 209.41; all 7 checkpoint
   files MD5-identical across runs AND identical to seburst's. DWCONV 53.94 /
   53.93 (-13.9, -20.4%; pre-registered 45-52 "as modeled" -- landed 1.9
   above it), PWCONV 136.39, GELU 13.02, ADD 5.05, SE 1.02 (flat). ONNX
   cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

DW refit on this run (R^2 0.998): 0.94 cyc/pixel (33.5) + 10.2 cyc/row (9.9;
was 23.5/22.8) + 291 cyc/channel (12.8; was 230/10.1 -- the two flat loops'
deeper fill/drain, ~+60 per channel) + ~0/output. Per-row delta -22 cycles on
the 48-channel layers, -9 on the 384-channel ones (few rows per channel, the
per-channel growth eats part of it). DW 53.9 = 33.5 (beats: 19.8 valid +
13.7 padding/stride) + 9.9 (row) + 12.8 (channel).

Cumulative on this latency line: 6,050ms -> ~215ms (-96.4%).
Operator split now: PW 63%, DW 25%, GELU 6.0%, ADD 2.4%, SE 0.5%.

Register map: UNCHANGED from wburst/worow (W_BURST 0x10c/0x110 present) --
only *_whoist-or-later ARM binaries are valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_flat.tcl (built with -DDWR_FLAT;
   since the default flip the flag-less build is the same design -- verified,
   run_csynth_default_check11.tcl)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_flat.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
4. ARM binaries: arm-linux-gnueabihf-gcc-13 on patrick@192.168.1.87,
   ~/a3_single_op_test_new (sources == fastvit_ip_v2/a3_single_op_test_src)
