mac_array_a3_dwob -- deployed baseline as of 2026-09-13 (ZHR-92)
==================================================================

Supersedes mac_array_a3_sohoist (2026-09-12: 1,095.36ms, LUT 82.81%, WNS
+0.138ns route-only).

Two source changes over sohoist, both now the DEFAULT build:

1. SHARED_MUL_ARMS (commit 83b4cdb, 2026-09-12): all seven remaining call
   sites on the top-level shared 32x32 multiplier removed -- PW_FLAT's
   in-pipeline (rt*MAC_PR+wr_row)*w_out -> rt_row_base + wr_row_off
   accumulators (the 2026-08-25 conversion redone, judged by arm count);
   ROW_READ oh*W -> accumulators; pw_ot_lo accumulator; the four per-chunk
   pw_ot_count products and scalar_hw/scalar_total -> narrow-typed
   multiplies on separate small cores. mul_32s_32s_32_2_1 now exists only
   inside DW's own dwr_consume3. On its own: WNS +0.128 (flat), the sink
   gone from the top-10, a different route-dominated path worst.
   (A first form using a per-chunk add-loop was rewritten by HLS's
   loop-idiom pass back into i32 multiplies on the shared unit -- see
   CLAUDE.md's technique list.)

2. DW_OUTPUT_BURST (port-reuse form, commit dfd309f + default flip this
   round): dwr_consume's per-lane 4-byte output packed into one ap_uint<32>
   write through PW_FLAT's existing out_burst port (no 6th gmem_act port,
   no new driver register). CROW_CCOL achieved II 8 -> 2. Had failed real
   P&R twice (-0.418 / -0.461ns) on the shared-multiplier sink that (1)
   removed; on top of (1) it closed at +0.172ns route-only. The wbuf
   ARRAY_PARTITION pragma rides along under the same macro; on its own it
   never changed II (2026-09-08) -- a companion, not an independent gain.
   Define DW_OUTPUT_BURST_OFF to get the old writeout back.

Files
-----
  mac_array_bd_wrapper_dwob.bit           raw Vivado bitstream,
                                          md5 75d47c4a83c83bb2d356b6b64c563a61
  mac_array_bd_wrapper_dwob_swapped.bin   byte-swapped, board-loadable; pulled from
                                          /lib/firmware/dwob2.bin after the board's
                                          own fpga_overlay.py Overlay class loaded it,
                                          md5 c5b7961792d2266c06f1537ac6ef949b
  timing_wide_dwob2.rpt                   300-path report, route_design alone
  utilization_dwob2.rpt

Deployed on the board as /lib/firmware/dwob2.bin (raw .bit at
/home/root/fpga/dwob2.bit). Golden rollback image /lib/firmware/
fastvit_bd_wrapper.bin NOT touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.172093ns   (sohoist +0.138; sma alone +0.128)
  LUT:  44,080/53,200 (82.86%)   (sohoist 44,054; sma 44,622 -> -542 for the
                                   DW mechanism, the third negative real delta
                                   for it: -280, -334, -542)
  BRAM: 106.5/140 (76.07%)
  DSP:  46/220 (20.91%)          (sohoist 56; the narrow cores are LUT multipliers)
  Worst path: gmem_act load unit read-data buffer (buff_rdata/dout_vld_reg ->
  raddr_reg, 9 logic levels, 7.41 of 9.57ns route) -- a member of the
  route-dominated population (+0.17..+0.45), not the mechanism.

Board verification (2026-09-13), pre-registered order
-----------------------------------------------------
1. entry5_dw single-op FIRST (the direct target), 3 runs: byte-exact
   0/196608, 38.9ms -> 24.90 / 24.95 / 24.71ms (-36%).
2. SE ops entry75/77/79/80 (Python reference, real desc_all.bin descriptors):
   all byte-exact. Controls entry3 PW 0/196608 (16.7ms), entry0 GELU
   0/786432 (3.4ms), entry10 ADD 0/196608 (3.0ms).
3. Full network, 82/82 written, two runs on the same bitstream:
   PL-side total 903.14 / 893.46ms  (sohoist 1,095.36: -17.5% / -18.4%;
   pre-registered interval 820-980)
   per-entry sums 873.24 / 873.11ms (identical; the PL-total spread is
   inter-entry host jitter)
   DWCONV 329.12 / 328.99ms  (sohoist 531.13: -38.0%; pre-registered 250-400)
   PWCONV 495.34 / 495.35   (-0.09%)   GELU 22.81 (-0.4%)   ADD 13.04 (0.0%)
   SE 12.93 (-0.15%)
   All 6 checkpoints + entry81 MD5-identical across both runs.
   ONNX float32 cosine: 0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 / -0.2811
   -- EXACT match to the established figures.

Cumulative on this latency line: 6,050ms (original) -> ~898ms = -85.2%.
Operator split now: PW 55%, DW 37%, GELU 2.5%, ADD 1.5%, SE 1.4% -- PW is
the single largest again.

Register map: UNCHANGED from sohoist / elemwise_burst+DW_IN_BURST (last
register DW_IN_BURST at 0x100/0x104). ARM binaries *_sohoist on the board
remain valid (md5 66fa3f26.. / 71acf8a9.. / 33ca9153..).

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_dwob2.tcl (built this bitstream
   with -DDW_OUTPUT_BURST on the command line; since the default flip the
   flag-less run_export_ip_sma.tcl form produces the same design -- verified
   by csynth totals + hw.h, see CLAUDE.md)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_dwob2.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
4. ARM binaries: arm-linux-gnueabihf-gcc-13 on patrick@192.168.1.87
