mac_array_a3_wburst -- deployed baseline as of 2026-09-15 (ZHR-92)
===================================================================

Supersedes mac_array_a3_seburst (same day: 272.5ms on the deferred-I/O harness /
PW 181.3 / DW 66.4 / WNS +0.254ns). All numbers on the busy-poll, deferred-I/O
ARM harness (*_whoist binaries -- REQUIRED, see Register map).

What changed (two flags, one mechanism, default ON this round):
  PW_WHOIST_WIDE (commit 9b6b3bc) -- a 32-bit hls::burst_maxi port w_burst on
    bundle gmem_w (the bundle's first burst_maxi) feeds PW_WEIGHT_HOIST one word
    per cycle instead of the byte-wide w_base[] copy (0.74 cycles/weight byte,
    21.2ms of PW). Requests chunked at ELEMWISE_CHUNK_WORDS; 4 byte-stores per
    iteration into pw_weight_cache, accepted at II=1 on its auto factor-2
    partition. Alignment verified on all 26 PW layers' chunk starts (mod4==0,
    cin multiple of 4), csim-asserted. Own register 0x10c/0x110.
  DWR_WBURST (commit 5797f02) -- REQUIRED by the first: once a 32-bit port
    exists on gmem_w the bundle's access width is 32-bit for every port, and
    w_base's byte reads in dwr_consume's prologue lost their K^2 kernel burst
    (kernel loop iteration latency 2 -> 15; real board DW +17.3ms, +183/+605
    cycles per channel at k=3/k=7 -- the PW_WHOIST_WIDE-only build,
    whoist, 269ms net -3.3, NOT promoted). Fix: the prologue reads each lane's
    kernel (K^2 bytes at an ARBITRARY byte offset -- co*K^2 is spread uniformly
    over mod 4 on the real layers) as ceil((ko+K^2)/4) words plus the shift
    byte as one word through the same w_burst (two back-to-back requests),
    words stored raw, bytes selected by (j+ko) in the copy loop with a running
    byte pointer (the kh*K+kw multiply is gone: dwr_consume DSP 17 -> 13).
    bias stays on gmem_b (32-bit, aligned, its own bundle -- checked).
  CLAUDE.md: the "plain pointer + burst_maxi coexist on a bundle" precedent is
  narrowed to pointers that do not depend on burst inference.

Files
-----
  mac_array_bd_wrapper_wburst.bit           raw, md5 c95b6bd10bf8c39603c37800de0c4f3f
  mac_array_bd_wrapper_wburst_swapped.bin   board-pulled /lib/firmware/dwwb.bin, md5 dc264c229e6fba1c31465ccf6555efae
  timing_wide_wburst.rpt, utilization_wburst.rpt

Deployed on the board as /lib/firmware/dwwb.bin (raw .bit at
/home/root/fpga/dwwb.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.180204ns   (seburst +0.254, whoist +0.088 -- placement rolls; worst
                       path gmem_act store-unit request FIFO, 84% route, the
                       route-dominated population)
  LUT:  46,366/53,200 (87.15%)   (seburst 46,014: +352; isolated said +298)
  BRAM: 110/140 (flat), DSP 26/220 (-4: the prologue's index multiply gone)

csim: with both flags raster 5/5, wiring 4/4, gelu_add 8/8, scalar_ops 4/4,
pw_weight_hoist 6/6, pw_scaling_probe 20/20. Flag-less after the default flips:
see the promotion commit (csynth totals bit-identical 252/22/39,991/78,034,
hw.h identical, six suites clean).

Board (2026-09-15, *_whoist binaries md5-verified on build server / local /
board before dispatch; pre-registered order, 30s timeouts)
------------------------------------------------------------------------
1. entry5_dw x3 (the side the wide port had broken): 2.924 / 2.929 / 2.925ms
   (seburst 2.915, whoist 3.000 -- restored); entry66 (3 x 144KB weight load)
   x3: 7.535 (seburst 10.779, -30%); all byte-exact.
2. Controls byte-exact: entry3 4.836 (2.3KB of weights: flat, as predicted),
   entry64 6.977 / entry72 6.337 (-31%), GELU 2.676, ADD 1.188, GAP 0.774,
   SCALE 0.176.
3. Full network 82/82, two runs: 253.07 / 253.03ms (seburst 272.5: -7.2%;
   pre-registered 248-252 -- 1ms over, exactly DW's residual); per-entry sums
   247.24 / 247.26; all 7 checkpoint files MD5-identical across runs AND
   identical to seburst's. PWCONV 160.59 (-20.67: 0.72 cycles per weight byte
   saved uniformly on all 26 layers), DWCONV 67.57 (+1.21 vs seburst: +27
   cycles/channel on BOTH k=3 and k=7 -- the second request + the 14-iteration
   word loop vs the old single K^2 burst; recoverable by issuing lane 1's
   requests before lane 0's copy loop, ~1ms, not done), GELU 13.02, ADD 5.05,
   SE 1.02 flat. ONNX cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.

DW refit on this run (R^2 0.998): 0.95 cyc/pixel (33.9) + 23.5 cyc/row (22.8) +
230 cyc/channel (10.1; was 196) + ~0/output. PW: 160.6 = 140.6 iterations +
19 requests + ~1 weights -- the weight term is gone.

Cumulative on this latency line: 6,050ms -> ~253ms (-95.8%).
Operator split now: PW 63%, DW 27%, GELU 5.1%, ADD 2.0%, SE 0.4%.

Register map: CHANGED -- W_BURST at 0x10c/0x110 appended (everything before it
unchanged). Only ARM binaries built from mac_array_driver.h at/after commit
9b6b3bc (*_whoist: full 100b06b3..., single-op 918dfc40..., add f2ae3b05...)
program it; older binaries (*_defer, *_busypoll, *_sohoist) leave w_burst's
base at 0 and will read weights from address 0 -- wrong output or a hang.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_dwwb.tcl (built with -DPW_WHOIST_WIDE
   -DDWR_WBURST; since the default flips the flag-less build is the same design --
   verified, run_csynth_default_check9.tcl)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_dwwb.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
4. ARM binaries: arm-linux-gnueabihf-gcc-13 on patrick@192.168.1.87,
   ~/a3_single_op_test_new (sources == fastvit_ip_v2/a3_single_op_test_src)
