A3 mac_array_top bitstream -- four-site loop-bound unification + pblock (option E)
====================================================================================

Archived 2026-08-22. See ZHR-92 (Linear) for the full round writeup (code
review -> option D attempted/reverted -> option E) and CLAUDE.md for two
lessons this round confirmed: (1) runtime-derived loop bounds synthesize
into real hardware arithmetic (DSP), not just comparators; (2) a
diagnosis-targeted pblock is not covered by this project's earlier
"no directive rotation" ban.

Source
------
fastvit_ip_v2/mac_array.cpp: the four-site runtime-loop-bound unification
(DW_WT_STAGE's kh/kw, DW_PATCH_STAGE's pr/pc, PW_STAGE's rr/cw,
PW_PATCH_HOIST's rr/cw -- all converted from runtime bounds to
compile-time bounds + a `valid` data bool, closing the same register-write
hazard class the earlier DW fix closed for cc). Option D (descriptor
local-copy into on-chip storage) was ATTEMPTED and REVERTED -- see the
comment in mac_array_top's dispatch loop; it failed placement outright
(real LUT need 66,317 vs 53,200 available) and was built on a wrong
diagnosis (fan-out, when the actual signature was placement distance).

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution4 (NOT solution5,
which was option D's failed attempt).

Vivado: vivado_impl/run_impl_mac_array_a3_sol4_pblock.tcl, with
pblock_mac_array_pre_place.tcl run automatically before place_design via
STEPS.PLACE_DESIGN.TCL.PRE. The pblock constrains the whole
mac_array_top_0 IP instance to SLICE_X0Y0:SLICE_X96Y149 (RAMB18/RAMB36/
DSP48 left at full device range) -- device bounds queried directly via
query_device_bounds.tcl, not assumed.

Files in this directory
------------------------
mac_array_bd_wrapper_pblock.bit           -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_pblock_swapped.bin   -- byte-swapped .bin, board-loadable via
                                              fpgautil (this is what was deployed)

Deployed to board as /lib/firmware/mac_array_pblock.bin (md5
61f83110fef73a85bef7e40fa6f937b6). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.165ns, route_design alone -- no phys_opt_design needed (better
margin than the earlier DW-fix-only round's +0.120ns, and unlike that
round's cache-revert cost, the pblock cost zero extra LUT). Critical path
confirmed OFF gmem_meta -- now run_layer's own address-multiply chain
(5 logic levels, 63% logic-dominated, DSP48E1=1/LUT3=1/LUT5=1/LUT6=1/
MUXF7=1 -- a normal, non-pathological path shape).
LUT:      35,402 / 53,200  (66.55%) -- unchanged vs. the unconstrained
          build (66.53%), confirming the pblock's cost was placement
          quality only, not resource overhead.
DSP48E1:  63
RAMB36E1: 10
RAMB18E1: 6

Board verification
-------------------
DW  (entry5, K=3 S=1, board_test_entry5_dw): 0/196,608 mismatches, byte-exact,
    113.19ms (up from the DW-fix-only round's 78.65ms -- the real,
    expected cost of the four-site unification: K=3 DW layers now run all
    49 weight-staging iterations instead of 9, a genuine structural
    trade-off, not a regression to chase)
PW  (entry3, board_test_entry3, regression): 0/196,608 mismatches, byte-exact,
    177.95ms (unchanged from the 179.38ms baseline within noise)

Two directions tried and abandoned this same round before option E, both
from reasoning ahead of the data rather than reading what was already on
record -- kept here as the honest trail, not hidden:
- Unified the four sites alone: route_design WNS dropped from the earlier
  round's +0.120ns to a phys_opt-rescued +0.004ns (essentially zero
  margin, not accepted).
- Option D (descriptor local-copy, theorized to fix gmem_meta via lower
  fan-out): placement failed outright. The fan-out theory was wrong -- the
  first-ever P&R run's own record already said "zero logic levels, 74%
  routing delay, fanout=70", which is a placement-distance signature, not
  a fan-out signature; that record was cited before option D was designed
  but not read carefully enough.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol4.tcl (fresh solution
   name if re-exporting -- do not reuse a solution across a second
   csynth+export without verifying the exported hdl/verilog is fresh, see
   CLAUDE.md's export_design staleness note)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol4_pblock.tcl
                  -nolog -nojournal
3. Bitstream:     vivado -mode batch -source run_bitstream_mac_array_a3_pblock.tcl
                  -nolog -nojournal   (writes .bit + .bin; byte-swap the
                  .bin with a 32-bit-word reversal before deploying)
