A3 mac_array_top bitstream -- run_layer rewrite stage 3, PW flat pipeline, X0-120 pblock
==========================================================================================

Archived 2026-08-24. See ZHR-92 (Linear) for the full round-by-round writeup. First
board-tested state of the run_layer rewrite (PW only -- DW untouched, stage 4 will
rewrite DW the same way).

What changed
------------
run_layer's entire PW (ot,cbase) loop + WRITEOUT_PW -- previously a nested-loop design
running STAGE->WSTAGE->GATHER->UNIFIED->WRITEOUT as separate named regions with a real
FSM round-trip between each -- replaced by pw_flat_pipeline(): one counter-driven
#pragma HLS PIPELINE II=1 loop per (rt,colt) tile, handling every ot/cbase internally.
Every derived address (channel offset, per-ot weight/output base, writeout row/col) is
a loop-carried accumulator with wrap-pair logic, not a computed index/mod (round 13's
`kh = step / MAX_K` mistake, avoided by construction, confirmed 0 divide/modulo
instances in the isolated stage-1 spike). DW is byte-for-byte unchanged -- still calls
run_reduce_dw (renamed from run_reduce_unified, PW's dead branch stripped) via its own
per-ot loop.

Found and fixed one real bug via csim during integration: the cbase-boundary wrap
incremented the channel-offset accumulator by a whole MAX_CIN_PW chunk instead of one
MAC_PD step, corrupting every real n_cbase>1 layer while n_cbase==1 shapes stayed
clean (Phase12/13 failed 8-15%, fixed by one line, also cleared two other phases
sharing the same root cause).

Achieved II=2 in the isolated stage-1 spike, II=3 once integrated into real run_layer
(csynth-confirmed) -- acc's cross-iteration dependency survives the compute<->writeout
phase transition as a real stall, worse than the spike predicted but still a real
theoretical win (~6-9x fewer cycles by the II=3/n_cbase model, vs. the spike's ~10-13x).

P&R: X0-96 (the last-confirmed-working size, chosen because solution26's csynth
estimate was only +1.3% over the prior baseline) FAILED to route -- 376 unroutable
pins, ALL tracing to one net (gmem_meta's AXI read-FIFO output bus), matching the
DW-burst round's own precedent exactly (design grew, pblock still sized for the
smaller pre-growth design). Widened X0-96->X0-120, one shot: routed clean, WNS
+0.140949ns, no phys_opt needed.

Real vs. estimated LUT, the round's other major finding: csynth ESTIMATED 52,060 LUT
(97%); real post-route is 30,669 LUT (57.65%) -- a 39.3-point gap, confirming (again)
that this design's csynth LUT estimate overshoots real P&R substantially at this point
on the curve. The isolated stage-1 spike's narrow "+4,412 LUT, unfavorable" comparison
was a large overestimate; LUT was never the real constraint.

Source
------
fastvit_ip_v2/mac_array.cpp: pw_flat_pipeline (new, ~150 lines) replaces PW's old
per-(rt,colt,ot,cbase) nested-loop compute+writeout. run_reduce_unified renamed
run_reduce_dw, PW branch stripped (DW-only now, single remaining call site).

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution26.
Vivado: vivado_impl/run_impl_mac_array_a3_sol26_pwflat_wide.tcl, with
pblock_mac_array_pre_place_wide.tcl (X0-120).

Files in this directory
------------------------
mac_array_bd_wrapper_pwflat.bit          -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_pwflat_swapped.bin  -- byte-swapped .bin, board-loadable
                                             (pulled directly from the board's
                                             /lib/firmware after load, not locally
                                             re-derived -- see the merge-round README
                                             for why local re-derivation doesn't match)

Deployed to board as /lib/firmware/mac_array_bd_wrapper_pwflat.bin (md5
0cee9a408bf904a95d44b40222394e36). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.140949ns, route_design alone -- no phys_opt_design needed. LUT: 30,669/53,200
(57.65%). DSP: (see csynth estimate, 63/220 28.64%, matched exactly -- not
independently re-measured post-route this round). Slice: 11,192/13,300 (84.15%).
Critical path still sourced from gmem_meta's AXI read-FIFO (zero logic levels, 73.8%
route-delay share) -- same persistent signature as every prior round on this line
(the original "Option E" pblock diagnosis); widening fixed the routability failure it
was diagnosed to fix, was never expected to relocate this structurally-persistent path.

Board verification
-------------------
PW (entry3, cin=cout=48, 64x64, 256 spatial tiles): 0/196,608 mismatches, byte-exact,
107.82ms (down from 133.74ms pre-rewrite, -19.4%). Real result is dramatically short
of the ~6-9x cycle-count projection from II=3 -- flagged explicitly as unresolved in
ZHR-92, not glossed over. Leading working hypothesis, not yet confirmed: entry3's real
shape needs 256 separate pw_flat_pipeline() calls (one per (rt,colt) tile, since the
rewrite only flattens WITHIN one tile's (ot,cbase,step) space, not across tiles per
the design doc's explicit scope decision) -- each call's own per-call overhead
(PW_PATCH_HOIST staging, function dispatch) may now dominate for a layer with this
many tiles, reintroducing at the tile level exactly the kind of round-trip cost the
rewrite eliminated at the (ot,cbase) level. Not yet measured directly.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol26.tcl (fresh solution name if
   re-exporting -- verify hdl/verilog freshness per CLAUDE.md's export_design
   staleness note; this export's component.xml vendor is xilinx.com, NOT user.org
   like earlier solutions -- check before reusing a VLNV string)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol26_pwflat_wide.tcl
                  -nolog -nojournal
3. Bitstream:     vivado -mode batch -source run_bitstream_mac_array_a3_sol26_pwflat.tcl
                  -nolog -nojournal   (writes .bit + .bin; the board's own
                  /home/root/fpga/fpga_overlay.py Overlay class does its own
                  byte-swap + /lib/firmware deploy from the raw .bit)
