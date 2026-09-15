mac_array_a3_seburst -- deployed baseline as of 2026-09-15 (ZHR-92)
===================================================================

Supersedes mac_array_a3_dwrow (same day: ~295ms on the busy-poll harness /
SE 9.27ms / WNS +0.211ns). ALL numbers here are on the busy-poll ARM harness
(commit 5b774ac, same day) -- do not compare against pre-busypoll totals
without subtracting 0.56ms x entry count (see CLAUDE.md, HARNESS CORRECTION).

What changed: SE_BURST (commit 89b7a76 + default flip this round; SE_BURST_OFF
reverts). The busy-poll data put the SE block's GAP at 7.63 and SCALE at 11.08
cycles/element (3.75 / 5.45ms) -- plain-pointer one-byte-per-iteration loops,
the exact shape run_gelu had at 8.6x before ELEMWISE_BURST; they were 9.2 of
SE's 9.27ms and the last un-bursted per-element path in the network. Same
mechanism, same ports (elemwise_in/out_burst on gmem_act; GAP/SCALE never run
concurrently with GELU/ADD), no new register:
  GAP   (reduction) -- read side only: chunked whole-plane burst with a
        loop-carried channel counter, per-channel sum/HW in a separate
        SEQUENTIAL loop (PIPELINE off: HLS's auto-pipelining had replaced the
        216-LUT sequential sdiv with a 2,512-LUT 47-stage one -- see the new
        CLAUDE.md rule), one byte per channel through out_base[] unchanged.
  SCALE (elementwise) -- gate vector burst-loaded once (cin bytes at in2_off,
        4-way partitioned buffer), then run_gelu's chunked read/compute/write
        with a channel counter selecting the gate.
Alignment verified on the real descriptors first (entries 75/80: in_off,
in2_off, out_off mod4==0, HW=64, cin=768); csim asserts guard cin <=
ELEMWISE_MAX_CH=1024, cin%4, HW%4 and the offsets.

Files
-----
  mac_array_bd_wrapper_seburst.bit           raw, md5 ad1bc0bbba6dacb2c9ca22e14967be7b
  mac_array_bd_wrapper_seburst_swapped.bin   board-pulled /lib/firmware/seburst.bin, md5 2e5107641ace7663da4c80053a57baf9
  timing_wide_seburst.rpt, utilization_seburst.rpt

Deployed on the board as /lib/firmware/seburst.bin (raw .bit at
/home/root/fpga/seburst.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.254457ns   (dwrow +0.211; worst path gmem_w load buffer -> DW
                       consume gather, 0 logic levels, 95% route -- the same
                       route-dominated member as the last three builds)
  LUT:  46,014/53,200 (86.49%)   (dwrow 45,224: +790; isolated said +1,929 --
                                  a DATAPATH delta, landed at 41% of isolated)
  BRAM: 110/140 (+3 tiles, as isolated), DSP 30/220 (-2, as isolated)

csim: with -DSE_BURST: scalar_ops_real_desc_tb 4/4 (GAP 0/768, SCALE 0/49152,
the direct check, independent Python reference), raster 5/5, wiring 4/4,
gelu_add 8/8, pw 6/6 + 20/20. Flag-less after the default flip: see the
promotion commit (csynth totals bit-identical 252/27/39,835/77,736, hw.h
identical, six suites clean).

Board (2026-09-15, busy-poll harness, pre-registered order, 30s timeouts)
------------------------------------------------------------------------
1. SE entries first, x3 each, byte-exact: entry75 GAP 3.749 -> 0.774ms (pre-
   registered ~0.5: 0.36 divide + 0.13 read + ~0.28 for 768 plain byte writes),
   entry80 SCALE 5.447 -> 0.176ms (~0.2), entry77 RELU 0.011, entry79 SIGMOID
   0.07 (unchanged).
2. Controls byte-exact and flat: entry5_dw 2.915, entry3 4.849, GELU 2.676,
   ADD 1.188, entry64 10.181.
3. Full network 82/82, two runs: PL total 287.46 / 287.12ms (dwrow/busypoll
   295.79 / 294.55: -2.8%; pre-registered 286-290); per-entry sums 266.72 /
   266.72; all 7 checkpoint files MD5-identical across runs AND identical to
   the dwrow bitstream's. SE 9.27 -> 1.02ms (-89%); DW 66.36, PW 181.27,
   GELU 13.02, ADD 5.05 all flat to 0.01ms. ONNX cosine EXACT
   0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

Cumulative on this latency line: 6,050ms -> ~287ms (-95.3%; the 6,050 was a
sleeping-poll measurement, so ~-94.9% like-for-like on the PL side).
Operator split now: PW 63%, DW 23%, GELU 4.5%, ADD 1.8%, SE 0.35%.
The SE block is done: 1.02ms, all four ops at or below 1us per channel-ish.

Register map: UNCHANGED (hw.h identical). *_busypoll board binaries are the
standard harness from this round on (*_sohoist kept for the A/B).

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_seburst.tcl (built with -DSE_BURST;
   since the default flip the flag-less build is the same design -- verified,
   run_csynth_default_check8.tcl)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_seburst.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
4. ARM binaries: arm-linux-gnueabihf-gcc-13 on patrick@192.168.1.87,
   ~/a3_single_op_test_new (sources == fastvit_ip_v2/a3_single_op_test_src)
