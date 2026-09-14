mac_array_a3_rowburst -- deployed baseline as of 2026-09-14 (ZHR-92)
=====================================================================

Supersedes mac_array_a3_dwob (2026-09-13: ~898ms, DW 329.0ms, WNS +0.172ns).

What changed: DWR_ROWBURST (commits 5079957 + default flip this round).
DW's packed 4-byte output write used to issue write_request + write +
write_response for EVERY word, all inside one CROW_CCOL iteration, so each
write waited ~30 cycles for its own B response inside the II=2 pipeline:
522,240 waits = ~156ms of DW's 329ms (real-board fit, 7.45 cycles/output;
mechanism confirmed in the schedule report: writereq ST_9, write ST_10,
5-stage writeresp ST_11-15 in one iteration). Now: one
write_request(row_addr, w_out/4) for lane 0 before the CCOL loop, write(word)
unchanged inside it, lane 1's words into a per-row buffer (<= DWR_ROWBUF_WORDS)
drained by a third per-row loop after CCOL (its request, its words, then both
responses -- AXI data order = AW order, no interleaving), then row base += w_out.
Side effect: one bus write per iteration -> CCOL achieved II 2 -> 1 (the
200-880 "two writes per iteration" dependence was a property of the lane
structure, not the hardware -- see CLAUDE.md's correction). Lane bases are
computed once per channel (no per-row multiply). Lane split is by unrolled
lane index (compile-time); buffer state lives within one CROW iteration.
Cost: CROW/CCOL no longer flattened (~20 cycles/row) + one B-wait per valid
lane-row -- ~44ms modelled over the network, against ~156ms removed.

Files
-----
  mac_array_bd_wrapper_rowburst.bit           raw, md5 03a4f00cf1772efcadb29c73fe482d79
  mac_array_bd_wrapper_rowburst_swapped.bin   board-pulled /lib/firmware/rowburst.bin,
                                              md5 9f666e56eeb8ea83a93bfe40b00319d1
  timing_wide_rowburst.rpt, utilization_rowburst.rpt

Deployed on the board as /lib/firmware/rowburst.bin (raw .bit at
/home/root/fpga/rowburst.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin
NOT touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.311772ns   (dwob +0.172; best since macpd4's +0.339)
  LUT:  44,243/53,200 (83.16%)   (dwob 44,080: +163; isolated csynth said +788)
  BRAM: 107/140 (76.43%)         (flat)
  DSP:  36/220 (16.36%)          (dwob 46: -10, exactly as isolated -- lane bases
                                  once per channel replaced per-write co*stride)
  Worst path: gmem_w load-unit data buffer -> DW consume gather (10 levels, 95%
  route), then descriptor-register / COPY_FROM_ROW / divider-reset paths --
  the route-dominated population; dwr_produce's carry chain is out of the top-5.

Board (2026-09-14), pre-registered order, 30s timeouts
------------------------------------------------------
1. entry5_dw first, 3 runs: byte-exact 0/196608, 24.8 -> 14.22 / 14.02 / 14.46ms (-43%).
2. SE ops 75/77/79/80 byte-exact (Python reference); controls entry3 PW 0/196608
   (16.9ms), entry0 GELU 0/786432 (3.4ms), entry10 ADD 0/196608 (5.2ms, polling noise).
3. Full network 82/82, two runs: PL total 791.96 / 789.26ms (dwob ~898: -12%);
   per-entry sums 769.97 / 769.28ms; all 7 checkpoint files MD5-identical.
   DWCONV 224.74 / 224.58ms (-31.7%; pre-registered 182-217 -> landed at the
   "II gain NOT realised" end); PWCONV 496.3 (+0.2%), GELU 22.9, ADD 13.1,
   SE 13.0 (all flat). ONNX cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

What the landing point says (fit on this run, R^2 0.995):
  DW = 6.08 cycles/input-pixel + 0.30 cycles/output + 90/channel
     (dwob: 4.72 / 7.45 / 160)
  The output term collapsed 156 -> 6ms (the mechanism worked); the per-pixel
  term is now 216 of DW's 225ms, ~6 cycles per input pixel uniformly across
  stride-1 and stride-2 layers, where consume at II=1 needs ~1.3-3.6. Produce
  (dwr_produce's plain per-pixel in_base[] AXI read) is now THE bottleneck --
  the input side is exposed by roughly (6.08-1) * 3.55M = ~180ms.

Cumulative on this latency line: 6,050ms -> ~790ms = -86.9%.
Operator split now: PW 63%, DW 28%, GELU 2.9%, ADD 1.7%, SE 1.6%.

Register map: UNCHANGED (hw.h identical to dwob/sohoist; last register
DW_IN_BURST 0x100). *_sohoist board binaries remain valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_rowburst.tcl (this bitstream was
   built with -DDWR_ROWBURST on the command line; since the default flip the
   flag-less build is the same design -- verified by csynth totals + hw.h)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_rowburst.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
