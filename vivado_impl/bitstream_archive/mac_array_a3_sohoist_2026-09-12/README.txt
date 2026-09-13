mac_array_a3_sohoist -- deployed baseline as of 2026-09-12 (ZHR-92)
=====================================================================

Supersedes mac_array_a3_elemwise_burst (2026-09-07: 1,099.77ms, LUT 83.22%,
WNS +0.017ns ONLY via phys_opt_design, route_design alone -0.167ns).

What changed: SCALAR_OP_SIZE_HOIST (commit fc6f6e0) -- a pure timing-margin
change with no datapath effect. The six scalar ops (RELU/SIGMOID/GELU/ADD/
GAP/SCALE) each computed their own element count in-body (10 multiply call
sites total, every one reachable only through mac_array_top's op_type
switch); HLS had bound them onto the top-level shared 32x32 multiplier
(mul_32s_32s_32_2_1, shared with run_layer) and put the op_type test into
that multiplier's operand mux. That multiplier is the critical-path sink of
every thin-margin build on this line. Now scalar_hw = h_in*w_in and
scalar_total = cin*scalar_hw are computed ONCE, unconditionally, in
mac_array_top before the switch and passed as int parameters.

Binding DB (mac_array_top.verbose.bind.rpt): top-level Multiplier opset went
from 8 op_type-predicated size multiplies to exactly two Predicate=true ops;
run_gelu/run_add own bind reports 4 -> 0 Multiplier ops each; exported RTL's
shared-multiplier operand mux 4 arms -> 2; one 32x32 multiplier instance
eliminated (4 -> 3).

Also in this build (source-level, no effect on the default build):
DW_OUTPUT_BURST gated OFF by default (dwr_writeout_packed + out_burst_w
param + wbuf partition pragma under one macro, same convention as
DWR_INPUT_BURST). Default DW path is source-identical to the prior
baseline's (verified vs commit 8868355; CROW_CCOL achieved II=8 = baseline).

Files
-----
  mac_array_bd_wrapper_sohoist.bit           -- Vivado bitstream (raw),
                                                md5 4005ac28f31267391e87740a27427adb
  mac_array_bd_wrapper_sohoist_swapped.bin   -- byte-swapped, board-loadable; pulled
                                                from the board's /lib/firmware/sohoist.bin
                                                after the board's own fpga_overlay.py
                                                Overlay class converted+loaded it,
                                                md5 a0dbd408b931a64599ad0fae9660b1ae
  timing_detail_sohoist.rpt                  -- top-10 paths, route_design alone
  utilization_sohoist.rpt

Deployed on the board as /lib/firmware/sohoist.bin (raw .bit at
/home/root/fpga/sohoist.bit). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin NOT touched (md5 7ee26f67a1fca38a2752e99cf0bac25b
re-verified this round).

Real P&R (route_design ALONE, phys_opt_design NOT run)
------------------------------------------------------
  WNS:  +0.138388ns   (prior baseline: -0.166790ns route-only, +0.017ns after phys_opt)
        -> +0.305ns improvement; first build on this line since macpd4 to close on
           route_design alone.
  LUT:  44,054/53,200 (82.81%)   (was 44,272, 83.22%, -218)
  BRAM: 107/140 tiles (76.43%)   (unchanged)
  DSP:  56/220 (25.45%)          (was 59, -3)
  Critical path: same sink (mul_32s_32s_32_2_1) and same source shape
  (run_layer FSM state, 4 logic levels) as the prior baseline, but data path
  8.707 -> 8.251ns: logic 5.287 -> 5.097 (a LUT6 in the operand mux became a
  LUT4 -- narrower mux with 2 arms instead of 4) AND route 3.420 -> 3.154
  (smaller cone). Structural, in the sink itself -- not a placement roll.

Board verification (2026-09-12)
-------------------------------
Pre-check on the PRIOR baseline bitstream (control, same binaries/bundles):
  entries 75/77/79/80 (GAP/RELU/SIGMOID/SCALE, real desc_all.bin descriptors,
  Python-computed reference from tools/gen_scalar_ops_csim_bundle.py) all
  byte-exact; entry3 byte-exact.  -> bundles and binaries are sound.

On THIS bitstream, single-op, all byte-exact:
  SE ops:  entry75 GAP 0/768, entry77 RELU 0/48, entry79 SIGMOID 0/768,
           entry80 SCALE 0/49152  (these four ops' signatures changed this
           round and had NO csim coverage before it -- new csim testbench
           scalar_ops_real_desc_tb.cpp 4/4, plus this board check)
  controls: entry3 PW 0/196608 (16.5ms), entry5_dw DW 0/196608 (38.9ms),
            entry0 GELU 0/786432 (3.3ms), entry10 ADD 0/196608 (2.2-2.9ms)
  (sub-ms SE ops show 1-6ms on the single-op timer -- ARM usleep polling
   jitter, identical spread on the prior baseline; not IP time)

Full network (82/82 written, two runs, same bitstream):
  PL-side total: 1,095.36ms / 1,094.50ms   (prior baseline 1,099.77ms: -0.4%, flat
                 as pre-registered -- this round changed multiply call sites, not
                 the datapath)
  Per-operator (run 2 vs prior baseline): DWCONV 531.13 (-0.01%), PWCONV 495.77
                 (-0.06%), GELU 22.90 (-0.13%), ADD 13.04 (-0.15%), SE 12.95 (0.00%)
  All 6 checkpoints + entry81 MD5-identical across the two runs.
  Six-checkpoint ONNX float32 cosine: 0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 /
  -0.2811 -- EXACT match to the project's long-established figures.

Register map: UNCHANGED from the elemwise_burst baseline + DW_IN_BURST at
0x100/0x104 (present in the IP since the DWR_INPUT_BURST round; the deployed
elemwise_burst bitstream itself predated it). ARM binaries on the board built
from current source this round: mac_array_single_op_test_sohoist,
mac_array_single_op_test_add_sohoist, mac_array_full_network_test_sohoist
(md5 66fa3f26.. / 71acf8a9.. / 33ca9153..).

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_sohoist.tcl
2. vivado_impl:   vivado -mode batch -source run_impl_bitstream_sohoist.tcl
                  (route_design only; phys_opt disabled; closes on its own)
3. Board:         scp the .bit to /home/root/fpga/, load via fpga_overlay.py's
                  Overlay class (does bit_to_bin + /lib/firmware copy + fpga_manager)
4. ARM binaries:  arm-linux-gnueabihf-gcc-13 on patrick@192.168.1.87,
                  ~/a3_single_op_test_new (sources == fastvit_ip_v2/a3_single_op_test_src)

Next round (pre-registered): re-enable DW_OUTPUT_BURST on top of this build as
its own single-variable round. It needed ~0.418ns of margin last time; this
build has 0.138 -- but the +0.305 shows the sink's mux structure had room, so
another call-site cleanup of the same kind is the candidate for the rest.
