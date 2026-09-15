mac_array_a3_dwrow -- deployed baseline as of 2026-09-15 (ZHR-92)
==================================================================

Supersedes mac_array_a3_merge4 (same day: ~378ms, DW 115.6ms, WNS +0.461ns).

What changed: DWR_ROW_PF + DWR_DEFER_WRESP (commit f06feca + default flip this
round), the DW per-ROW fixed cost. Step 0 first: the merge4 refit's "886
cycles/channel" was NOT a per-channel cost -- adding a per-row (h_pad) term
takes the DW fit from R^2 0.879 to 0.977 and the per-channel term collapses to
~0: DW 115ms = 0.89 cycles/input-pixel + 0.59/output + ~73 cycles per padded
row x 97,344 rows (~71ms). The consume prologue (bias/shift/kernel loads) is
already burst-inferred and hidden -- not the lever. Per row, both sides of the
DATAFLOW pair paid one AXI latency that OVERLAP each other (dwr_produce issued
the row's read_request only after pushing the previous row's last beat and
then blocked ~35 cycles on the first read(); dwr_consume popped the row's
write_response() right after its last write(), ~30 cycles), so removing one
side alone only exposes the other (the ROWBURST -> ROWREAD lesson) -- both
were built and measured TOGETHER by decision:
  DWR_ROW_PF      -- dwr_produce issues row r+1's read_request at the START of
                     row r (row 0 primed before ROW), read() unchanged inside
                     the pipelined COL loop; <= 2 outstanding, <= 64 words
                     buffered (adapter 16 / 256). Same shape as
                     PW_ROWREAD_PREFETCH.
  DWR_DEFER_WRESP -- dwr_consume pops a valid row's fpg B responses two valid
                     rows later (DWR_WRESP_DEFER_ROWS=2, <= 4 in flight vs 16),
                     flat one-response-per-iteration drain after CROW (the
                     first drain form, g loop inside a row loop, was 200-885
                     II=2 -- fixed). Same shape as PW_DEFER_WRESP.
Accumulator addressing, no new multiply (binding DB: dwr_produce2 2 /
dwr_consume3 12 multiplier ops, unchanged; 32x32 only in dwr_consume3).
Step-1 schedule: readreq only in produce's ROW body, read only in
Pipeline_COL (II=1); writereq in consume's row body, write in CCOL (II=1) /
L1_DRAIN, writeresp only in the per-row response loop + drain -- no data op
left its pipeline; ZERO II violations design-wide; produce per-row FSM states
unchanged (12). DWR_ROW_PF_OFF / DWR_DEFER_WRESP_OFF revert.

Files
-----
  mac_array_bd_wrapper_dwrow.bit           raw, md5 625412b3f1d3b58225618797f3388e25
  mac_array_bd_wrapper_dwrow_swapped.bin   board-pulled /lib/firmware/dwrow.bin, md5 eef672e52889e22aa12a0585a67fbb79
  timing_wide_dwrow.rpt, utilization_dwrow.rpt

Deployed on the board as /lib/firmware/dwrow.bin (raw .bit at
/home/root/fpga/dwrow.bit). Golden /lib/firmware/fastvit_bd_wrapper.bin NOT
touched (md5 7ee26f67a1fca38a2752e99cf0bac25b).

Real P&R (route_design ALONE, no phys_opt)
------------------------------------------
  WNS:  +0.210849ns   (merge4 +0.461 -- placement roll; worst path gmem_w load
                       buffer -> DW consume gather, 0 logic levels, 96% route,
                       the route-dominated population, not the new code; next
                       five at +0.248..+0.283)
  LUT:  45,224/53,200 (85.01%)   (merge4 44,590: +634; isolated said +626 --
                                  the closest isolated/real match on this line)
  BRAM: 107/140, DSP 32/220     (flat)

csim: with the flags (probe round) raster 5/5, wiring 4/4; flag-less after the
default flip: raster 5/5, wiring 4/4, gelu_add 8/8, scalar_ops 4/4,
pw_weight_hoist 6/6, pw_scaling_probe 20/20 (see the promotion commit).

Board (2026-09-15), pre-registered order, 30s timeouts
------------------------------------------------------
1. entry5_dw x3: byte-exact, 3.59 / 3.95 / 4.56ms (merge4 ~5.5; pre-registered
   ~4.0; 1ms poll granularity).
2. Controls byte-exact and flat: entry3 5.46, entry0 GELU 3.32, entry10 ADD
   2.28, entry64 / entry66 10.9, SE ops 75/77/79/80 PASS.
3. Full network 82/82, two runs: PL total 343.21 / 342.65ms (merge4 ~378:
   -9.3%; pre-registered 335-345); per-entry sums 321.1 / 321.2; all 7
   checkpoint files MD5-identical. DWCONV 78.96 / 78.92ms (-31.7%; the
   pre-registered 68-85 "both latencies hidden" band); PWCONV 193.33, GELU
   22.84, ADD 13.03, SE 12.95 (flat). ONNX cosine EXACT
   0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811.
   Landing vs model: -36.6ms = 37.6 cycles per padded row over 97,344 rows,
   i.e. per-row 73 -> ~35 (pre-registered 25-40). Layer 1 (largest, least
   quantized, 6,240 rows): per-row residual 72.9 -> 21.2.

MEASUREMENT CAVEAT (new this round, applies to every per-layer fit): the
full-network harness's per-entry "done:" times are quantized to ~1.08ms --
every value is a multiple of 1.08/1.09 (its usleep(500) poll sleeps ~1.08ms
on this kernel). Totals are fine; per-layer DW values (2.16/3.24/4.33ms) carry
+-50k cycles each, so the per-row vs per-pixel split is resolution-limited
from this harness (refit: ~54 cycles/row, 0.86/pixel, R^2 0.968 --
indicative only). A finer poll is a driver-only change (no bitstream) and is
the prerequisite for decomposing DW again or the small ops (GELU/ADD/SE).

Where DW's remaining ~79ms is, roughly: ~30ms pixel floor (II=1 over 3.55M
padded pixels) + ~50ms per-row FSM glue (produce's 12-state ROW body incl.
the 8-cycle readreq op, CCOL/COL fill-drain, per-lane request/response
loops) + fpg=2 lane-1 drain.

Cumulative on this latency line: 6,050ms -> ~343ms = -94.3%.
Operator split now: PW 56%, DW 23%, GELU 6.7%, ADD 3.8%, SE 3.8%.

Register map: UNCHANGED (hw.h identical). *_sohoist board binaries remain valid.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_dwrow.tcl (built with -DDWR_ROW_PF
   -DDWR_DEFER_WRESP; since the default flip the flag-less build is the same
   design -- verified, run_csynth_default_check7.tcl)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_dwrow.tcl
3. Board: scp .bit to /home/root/fpga/, load via fpga_overlay.py Overlay
