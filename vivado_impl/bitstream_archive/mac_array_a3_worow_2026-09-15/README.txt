mac_array_a3_worow -- deployed baseline as of 2026-09-15 (ZHR-92)
==================================================================

Supersedes mac_array_a3_wburst (same day: ~253ms / PW 160.6 / DW 67.6 / WNS
+0.180ns). All numbers on the busy-poll, deferred-I/O ARM harness (*_whoist
binaries; register map unchanged from wburst).

What changed: PW_WRITEOUT_ROW (commit 0f707c2 + default flip this round;
PW_WRITEOUT_ROW_OFF reverts). Decomposing the 131ms PW_FLAT iteration floor on
the real network: useful compute 90.0ms (68.6%; 0.576 GMAC at 64 MAC/cycle) +
channel-padding waste 4.9 (3.7%) + WRITEOUT iterations 36.4 (27.7%; 50% of the
iterations on cin=48 layers). The writeout phase was 16 iterations per (ot,
tile), one output byte each through a single time-multiplexed clip_shift. The
FAST instance now finishes a whole 4-byte row per iteration (4 clip_shift lanes,
16 acc reads from the complete-partitioned register array): 16 -> 4 iterations,
2,728,512 fewer network-wide. The write_request+write stay in the writeout
iteration (one word per iteration); PW_DEFER_WRESP's response pop moved to the
COMPUTE phase (last cbase, k=4..7, one per iteration while > MAC_PR pending) --
no writeout iteration is free of a bus write any more, and the pop must not
share an iteration with the request/write; pending oscillates 8 -> 4 -> 8 (the
ot two back), pop-to-push distance >= 32 cycles at cin=48. The NARROW instance
(the two W=1 SE fc layers) keeps the 16-step byte form; run_layer's
iters_per_ot selects 4 vs 16 by the same layer-constant predicate as the
dispatch.

Files
-----
  mac_array_bd_wrapper_worow.bit           raw, md5 d42608ec64a9380e2a3f8997ffe29e37
  mac_array_bd_wrapper_worow_swapped.bin   board-pulled /lib/firmware/worow.bin, md5 0041f5f068316a9b219a909e0be0a23c
  timing_wide_worow.rpt, utilization_worow.rpt

Deployed on the board as /lib/firmware/worow.bin (raw .bit at
/home/root/fpga/worow.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.284796ns   (wburst +0.180; worst path gmem_act store-unit response
                       FIFO, 80% route -- the route-dominated population)
  LUT:  46,144/53,200 (86.74%)   (wburst 46,366: -222; isolated said +1,008 --
                                  the writeout FSM shrank more than 4 lanes cost)
  BRAM: 110/140, DSP 26/220     (flat)

csim: with the flag pw_weight_hoist 6/6, pw_scaling_probe 20/20, raster 5/5,
wiring 4/4, gelu_add 8/8, scalar_ops 4/4; flag-less after the default flip:
see the promotion commit (csynth totals bit-identical 252/22/39,475/79,042,
hw.h identical, six suites clean).

Board (2026-09-15, *_whoist binaries md5-verified; pre-registered order)
------------------------------------------------------------------------
1. entry3 x3 (cin=48 -- writeout was half its iterations): 4.836 -> 3.782 /
   3.897 / 3.783ms (-22%; pre-registered ~-25%); entry66 x3 (cin 1152, 5%
   writeout): 7.535 -> 7.352 (-2.4%; ~-4%); byte-exact.
2. Controls byte-exact: entry5_dw 2.924, GELU 2.676, ADD 1.189, GAP 0.773,
   SCALE 0.178 (flat); entry60 2.145, entry64 6.425 (PW, down as expected).
3. Full network 82/82, two runs: 228.98 / 228.82ms (wburst 253.07: -9.6%;
   pre-registered 226-228); per-entry sums 223.27 / 223.05; all 7 checkpoint
   files MD5-identical across runs AND identical to seburst's. PWCONV 136.39
   (-24.20, -15.1%; pre-registered 132-137 "as modeled"), DWCONV 67.80 / 67.56,
   GELU 13.01, ADD 5.05, SE 1.02 (flat). ONNX cosine EXACT
   0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

PW refit on this run (R^2 0.975): 1.121 cyc/iteration (116.6; the new floor is
104.0 = 131.2 - 27.3) + 26 cyc/request (12.4) + 0.75 cyc/word (7.5). PW 136.4
= 104 (iterations) + ~18 (serial input read) + ~14 (request/fill overheads).
What is left in PW vs the 90ms algorithmic minimum: 4.9 channel padding, 9.1
remaining writeout iterations (would need the writeout overlapped with the next
ot's compute -- an FSM restructure), ~18 serial ROW_READ (would need a
DATAFLOW-class ping-pong), ~14 overheads.

Cumulative on this latency line: 6,050ms -> ~229ms (-96.2%).
Operator split now: PW 60%, DW 30%, GELU 5.7%, ADD 2.2%, SE 0.4%.

Register map: UNCHANGED from wburst (W_BURST 0x10c/0x110 present) -- only
*_whoist-or-later ARM binaries are valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_worow.tcl (built with -DPW_WRITEOUT_ROW;
   since the default flip the flag-less build is the same design -- verified,
   run_csynth_default_check10.tcl)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_worow.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
4. ARM binaries: arm-linux-gnueabihf-gcc-13 on patrick@192.168.1.87,
   ~/a3_single_op_test_new (sources == fastvit_ip_v2/a3_single_op_test_src)
