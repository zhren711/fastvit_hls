mac_array_a3_rowread -- deployed baseline as of 2026-09-14 (ZHR-92)
====================================================================

Supersedes mac_array_a3_rowburst (same day: ~790ms, DW 224.6ms, WNS +0.312ns).

What changed: DWR_ROWREAD (commit c659a97 + default flip this round) -- the
input-side twin of DWR_ROWBURST. After ROWBURST removed the output side's
per-write B-waits, the real-board refit put DW at 6.08 cycles per INPUT pixel
(216 of 225ms) with consume at II=1 needing ~1.3-3.6: dwr_produce's plain
per-pixel in_base[] AXI read was the DW bottleneck. Now: one
read_request(row_addr, w_in/4) per in-image row BEFORE the COL loop; the
word-packed read() stays INSIDE the pipelined COL loop (one 32-bit read per 4
in-image pixels, byte peeled off by >>8 per pixel); reads stay overlapped with
consume through the DATAFLOW pair. NOT DWR_INPUT_BURST's shape (serial
per-channel prefetch, additive, measured +43ms). Port: dw_in_burst -- left
behind by the rejected DWR_INPUT_BURST round with its driver register (0x100)
already wired: zero interface change. Alignment verified first: all 79,872
real DW input row starts are mod4==0; padding never touches addressing.
csynth: readreq only in the ROW body, read() only in Pipeline_COL, COL
achieved II=1 (iteration latency 12 -> 3), ZERO II violations design-wide
(first time on this line). DWR_ROWREAD_OFF reverts.

Files
-----
  mac_array_bd_wrapper_rowread.bit           raw, md5 eeb00b07319e46045c8f44f5e19633fc
  mac_array_bd_wrapper_rowread_swapped.bin   board-pulled /lib/firmware/rowread.bin, md5 0eca8345da65caf89f22c6f78a068d52
  timing_wide_rowread.rpt, utilization_rowread.rpt

Deployed on the board as /lib/firmware/rowread.bin (raw .bit at
/home/root/fpga/rowread.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin
NOT touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.112692ns   (rowburst +0.312 -- a placement roll on the SAME worst
                       structure: gmem_w load buffer -> DW consume gather,
                       12 levels, 96% route; not the new read code)
  LUT:  44,422/53,200 (83.50%)   (rowburst 44,243: +179; isolated said -1,005 --
                                  opposite sign, another data point)
  BRAM: 107/140 (76.43%)         (flat)
  DSP:  32/220 (14.55%)          (rowburst 36: -4, exactly as isolated)

Board (2026-09-14), pre-registered order, 30s timeouts
------------------------------------------------------
1. entry5_dw first, 3 runs: byte-exact 0/196608, 14.2 -> 5.44 / 5.65 / 5.72ms (-60%;
   38.9ms three rounds ago).
2. SE ops 75/77/79/80 byte-exact; controls entry3 PW 0/196608 (17.1ms), entry0 GELU
   0/786432 (3.1ms), entry10 ADD 0/196608 (2.5ms).
3. Full network 82/82, two runs: PL total 679.35 / 679.43ms (rowburst ~790: -14%;
   pre-registered 670-720); per-entry sums 659.94 / 659.36; all 7 checkpoint files
   MD5-identical. DWCONV 115.56 / 115.51ms (-48.6%; pre-registered 100-150 =
   "input-side handshake was the bottleneck, same mechanism, same fix");
   PWCONV 495.3 (-0.1%), GELU 22.8, ADD 13.1, SE 13.0 (flat).
   ONNX cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

Refit on this run (R^2 0.88): DW = 1.29 cycles/input-pixel + 1.30 cycles/output
+ 886/channel = 46 + 27 + 39ms (history: dwob 4.72/7.45/160 -> rowburst
6.08/0.30/90 -> here). The pixel term is at the II=1 floor (35.5ms); what is
left is three small terms, the per-channel one (weight/bias loads as individual
reads, dataflow start, per-row overheads) now the largest.

Cumulative on this latency line: 6,050ms -> ~679ms = -88.8%.
Operator split now: PW 73%, DW 17%, GELU 3.4%, ADD 1.9%, SE 1.9%.

Register map: UNCHANGED (hw.h identical; last register DW_IN_BURST 0x100, now
actually used by DW). *_sohoist board binaries remain valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_rowread.tcl (built with -DDWR_ROWREAD;
   since the default flip the flag-less build is the same design -- verified by
   csynth totals + hw.h)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_rowread.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
