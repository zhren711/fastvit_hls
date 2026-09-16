mac_array_a3_preg -- deployed baseline as of 2026-09-17 (ZHR-92)
=================================================================

Supersedes mac_array_a3_dwflat (2026-09-16: 215.14ms / DW 53.9 / PW 136.4 / WNS
+0.135ns / LUT 85.74%). All numbers on the busy-poll, deferred-I/O ARM harness
(*_whoist binaries; register map UNCHANGED -- the same *_whoist-or-later binaries).

This is a TIMING-MARGIN promotion that also came out ahead on latency: it is the
100MHz build of the source that closes 9.0ns/111MHz on route_design alone.

What changed (four flags, all ON by default; each has an _OFF revert or =2):
  CTR_NARROW        (dcf26d0)  the four loop-carried counter chains named by the
                               9.0/8.5/8.0ns frequency sweep (PW_FLAT w_pending/k/
                               cbase_idx, ROW_READ_FILL4 col_w/widx/cur_row,
                               DWR_CONSUME_FLAT l1_nbuf) narrowed to real widths,
                               one net update per iteration: HLS estimates
                               9.30/8.20/7.80/7.39 -> 7.60/7.78/7.30/7.30.
  PW_MAC_PREG       (0a13a9f)  the PW MAC product registered (BIND_OP op=mul
                               impl=fabric latency=1 -> mul_8s_8s_16_2_1; PW_FLAT
                               II=1, FAST depth 9 -> 11). Splits the operand-reg ->
                               LUT 8x8 multiply -> 32-bit add -> acc chain (14
                               levels, 8.25-8.94ns, 196 of the 9.0ns top-300) into
                               multiply | add. The HLS Estimated never saw this chain
                               (7.601 before and after) -- P&R is the only judge.
  PW_ACC_NARROW     (0a13a9f)  per-lane accumulators ap_int<26> (real max 16,384 x
                               288 = 4.7M; csim-asserted cin < 8192).
  PW_DEFER_WRESP_KEEP_OTS=3 (b6b8535)  the deferred write-response pop takes the
                               ot THREE back (<= 12 in flight vs the adapter's 16)
                               instead of two. Found by the board round of the first
                               three: +0.75ms network, ALL on the three cin=48 PW
                               layers -- the pop-to-write distance (32 iterations
                               minus the write/writeresp stage gap, ST_7 -> ST_10 as
                               the pipeline deepened) had dropped from 29 to 26
                               effective cycles against a ~30-35 cycle B response.
                               With +20 iterations of distance those layers no
                               longer stall at all -- and dwflat itself had been
                               stalling ~3.4 cycles per pop on them.

Files
-----
  mac_array_bd_wrapper_preg.bit            raw, md5 f6d778d839b23bbabb48ccc87f896f96
  mac_array_bd_wrapper_preg_swapped.bin    board-pulled /lib/firmware/keep3.bin,
                                           md5 4b7a511a46d94077d4700c43d501f5ad
  timing_wide_preg.rpt, utilization_preg.rpt            (10.0ns / 100MHz, deployed)
  timing_wide_preg_9p0ns_111MHz.rpt, utilization_preg_9p0ns_111MHz.rpt
                                           (9.0ns / 111MHz P&R of the same source;
                                            NOT deployed -- FCLK0 is boot-fixed)

Deployed on the board as /lib/firmware/keep3.bin (raw .bit at /home/root/fpga/
keep3.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin md5 7ee26f67... -- see the
incident note below.

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  10.0ns: WNS +0.380631ns   (dwflat +0.135; worst path gmem_act load-unit read
                             buffer -> ROW_READ_FILL4, 0 logic levels, 73% route)
          LUT 44,196/53,200 (83.08%; dwflat 45,614: -1,418)
          FF  ~39.6k (+2.6k: the 64x2 product registers + deeper PW_FLAT stages)
          BRAM 111/140, DSP 28/220
  9.0ns / 111MHz: WNS +0.073ns (the preg build without KEEP_OTS=3 was +0.146; a
          placement roll on the route-dominated population -- worst path PW_FLAT
          add -> LayerDescV2 RAM WE, 7 levels, 73% route). LUT 44,884 (84.37%).

csim: six suites flag-less: raster 5/5, wiring 4/4, gelu_add 8/8, scalar_ops
4/4, pw_weight_hoist 6/6, pw_scaling_probe 20/20. Flag-less csynth bit-identical
to the tested export (255/22/40,847/78,124, hw.h, per-module estimates, PW_FLAT
II=1 depth 11, zero II violations).

Board (2026-09-17, after a full power cycle -- see below; *_whoist binaries
md5-verified; pre-registered order)
------------------------------------------------------------------------------
1. flat (known good) entry5_dw 2.228 / entry3 3.781 as the clean-board control.
2. keep3 entry5_dw 2.235 x2 byte-exact (the IP-vs-PS decider: PASS).
3. entry3 3.359 / 3.358 / 3.358 (dwflat 3.783: -11%; the A/B judge, target was
   <= 3.783), byte-exact.
4. Controls byte-exact and flat: entry66 7.281, entry64 6.398, entry60 2.137,
   GELU 2.675, ADD 1.187, GAP 0.773, RELU 0.011, SIGMOID 0.072, SCALE 0.176.
5. Full network 82/82, two runs: 212.34 / 212.21ms (dwflat 215.14: -2.9ms,
   -1.4%; pre-registered ~214.4 -- landed better); per-entry sum 206.04; all 7
   checkpoint files MD5-identical across runs AND identical to dwflat's; ONNX
   cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811. PWCONV 132.98
   (-3.41), DWCONV 53.97, GELU 13.02, ADD 5.05, SE 1.02 (flat).
   Per PW layer vs dwflat: all 26 flat or faster; the three cin=48 layers
   (entries 3/7/13) -0.425 / -1.286 / -1.285 = -14 cycles per (ot, tile) --
   i.e. dwflat was stalling ~3.4 cycles per pop on them; the big-cin chunked
   layers -0.02..-0.07 (the product register / narrow acc).

Cumulative on this latency line: 6,050ms -> ~212ms (-96.5%).
Operator split: PW 63%, DW 25%, GELU 6.1%, ADD 2.4%, SE 0.5%.

INCIDENT (2026-09-16 evening, before this board round)
------------------------------------------------------
The first attempt to board-test this bitstream hit the poisoned-PS state: the
SSH session dropped at the Overlay load, entry3 passed x3 but entry5_dw FAILED
and the next dispatches crashed ("stack smashing detected", dmesg "Alignment
trap: sh"), and /lib/firmware/keep3.bin read back a different md5 on successive
reads. After the power cycle the board copy of keep3.bit had md5 b4d5958f...,
NOT the local f6d778d8... -- the scp had landed corrupted BEFORE the load, i.e.
the PS was already corrupting writes while idle on flat.bin; the bitstream then
loaded was garbage (partially functional: PW passed, DW failed). Redeployed from
the local build after the power cycle: everything above. The IP was never at
fault; third occurrence of the unexplained PS-corruption class.
Also in the recovery: loading the golden .bit via Overlay OVERWRITES
/lib/firmware/fastvit_bd_wrapper.bin (the checklist caveat) -- and the .bit
files named fastvit_bd_wrapper on the board and in petalinux/hardware are NOT
the golden image (they swap to 9f1b98a9...). The golden .bin was restored from
/home/root/fpga_unified_v18gelu/fastvit_bd_wrapper.bin (md5 7ee26f67..., the
only copy) and is now also archived locally at
vivado_impl/bitstream_archive/_golden_v18gelu/. For proof-of-life use
"echo fastvit_bd_wrapper.bin > /sys/class/fpga_manager/fpga0/firmware" (loads
the existing .bin), never Overlay() on a .bit of that name.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_keep3_100.tcl (flag-less)
2. vivado_impl: vivado -mode batch -source run_impl_keep3_100.tcl
   (run_impl_keep3_90.tcl for the 111MHz P&R)
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay, verify
   the .bit md5 on the board BEFORE loading
4. ARM binaries: unchanged (*_whoist)
