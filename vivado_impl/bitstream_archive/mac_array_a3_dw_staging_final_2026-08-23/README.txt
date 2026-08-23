A3 mac_array_top bitstream -- DW staging optimization line, closed
====================================================================

Archived 2026-08-23. See ZHR-92 (Linear) for the full round-by-round
writeup of this line: PIPELINE attempt (failed) -> byte-throughput
accounting -> three DW-row-buffer redesigns (whole-row dead end, single
flat-loop burst-inference failure, two-path fast/edge split that worked
but regressed real latency by reading 14.2x wasted bytes) -> revert ->
HW Interfaces bit-width table (gmem_act/gmem_w both 8-bit burst payload
despite a 32-bit AXI bus; gmem_w never bursts at all, gmem_b/gmem_meta
are genuinely 32-bit) -> DW_WT_STAGE dual-master-interleaving diagnosis
(exact log message: "Unable to schedule bus request operation ... due to
limited memory ports") -> split into DW_BT_STAGE/DW_WT_STAGE (II 49->1)
-> MAX_K->K revert (98->18 iterations, protected by GATHER_ALL_DW's
weight-side valid mask) -> DW_PATCH_STAGE PATCH_R_MAX/PATCH_C_MAX->
patch_r/patch_c revert (same protection mechanism, verified term-by-term)
-> pblock re-widening (X0-96 was sized for a bigger design; after LUT
dropped 66.53%->55.07% the same pblock stopped closing timing at
-0.003ns; widening to X0-120 fixed it on the first try, same fix as the
earlier DW-burst routing failure -- confirms pblock sizing must track
LUT occupancy, not be set once).

Source
------
fastvit_ip_v2/mac_array.cpp: DW_WT_STAGE split into DW_BT_STAGE (gmem_b
only) + DW_WT_STAGE (gmem_w only, kh/kw bounded by runtime K, protected
by GATHER_ALL_DW's valid mask -- comment explicitly forbids removing
that mask without re-closing this bound). DW_PATCH_STAGE's pr/pc bound
reverted from PATCH_R_MAX/PATCH_C_MAX to runtime patch_r/patch_c, same
protection, same warning against shrinking dw_patch's declared size.

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution14.
Vivado: vivado_impl/run_impl_mac_array_a3_sol14_pblock_wide.tcl, with
pblock_mac_array_pre_place_wide.tcl (X0-120, widened from the original
X0-96) run via STEPS.PLACE_DESIGN.TCL.PRE.

Files in this directory
------------------------
mac_array_bd_wrapper_final.bit           -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_final_swapped.bin   -- byte-swapped .bin, board-loadable
                                             (this is what was deployed)

Deployed to board as /lib/firmware/mac_array_final.bin (md5
f4df55af69359aa67c73c577c1e2461b). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.113ns, route_design alone -- no phys_opt_design needed. Critical
path off gmem_meta (run_layer's own address-multiply chain, 5 logic
levels -- a normal path shape). LUT: 29,302 / 53,200 (55.08%) -- well
down from the 66-67% of the immediately preceding rounds in this same
line. DSP48E1: 70. RAMB36E1/18E1: 10/6.

Board verification
-------------------
DW  (entry5, K=3 S=1, board_test_entry5_dw): 0/196,608 mismatches, byte-exact,
    71.24ms (down from 113.19ms at the start of this optimization line,
    -37%)
PW  (entry3, board_test_entry3, regression): 0/196,608 mismatches, byte-exact,
    178.20ms (unchanged from baseline within noise)

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol14.tcl (fresh
   solution name if re-exporting -- verify hdl/verilog freshness, see
   CLAUDE.md's export_design staleness note)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol14_pblock_wide.tcl
                  -nolog -nojournal
3. Bitstream:     vivado -mode batch -source run_bitstream_mac_array_a3_final.tcl
                  -nolog -nojournal   (writes .bit + .bin; byte-swap the
                  .bin with a 32-bit-word reversal before deploying)
