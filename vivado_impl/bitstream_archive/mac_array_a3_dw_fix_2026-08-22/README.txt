A3 mac_array_top bitstream -- DW loop-bound fix + pw_weight_cache revert
==========================================================================

Archived 2026-08-22. See ZHR-92 (Linear) for the full round writeup and
CLAUDE.md for the export_design staleness lesson this build exposed.

Source
------
Built from git commit 38e7001 ("A3: fix DW's runtime-loop-bound timing
hazard, revert PW weight-cache for BRAM headroom"), fastvit_ip_v2/mac_array.cpp,
HLS solution "solution3" under fastvit_ip_v2/mac_array_poc_a3_axi/ (solution2
was NOT reused -- its export_design output was stale, see CLAUDE.md).

Files in this directory
------------------------
mac_array_bd_wrapper_dw_fix.bit            -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_dw_fix_swapped.bin    -- byte-swapped .bin, board-loadable via
                                               fpgautil (this is what was deployed)
mac_array_bd_wrapper_routed_physopt.dcp    -- post-route, post-phys_opt checkpoint
                                               (phys_opt_design found zero setup
                                               violations -- this checkpoint IS the
                                               phys_opt result, not just raw route)

Deployed to board as /lib/firmware/mac_array_dw_fix.bin (md5
b42ae9179d07ffca5b4f5461adab56c0). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.120ns, route_design alone -- phys_opt_design ran and found 0 violations
to fix (design converges cleanly, not a phys_opt rescue).
LUT:      31,718 / 53,200  (59.62%)
FF:       40,146 / 106,400 (37.73%)
RAMB36E1: 10
RAMB18E1: 6
DSP48E1:  90

Board verification
-------------------
DW  (entry5, K=3 S=1, board_test_entry5_dw): 0/196,608 mismatches, byte-exact,
    78.65ms
PW  (entry3, board_test_entry3, regression): 0/196,608 mismatches, byte-exact,
    178.94ms (unchanged from the 179.38ms baseline)

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol3.tcl (or bump to a new
   solution name -- do not reuse solution3 for a second export without
   verifying the exported hdl/verilog is fresh, see CLAUDE.md)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol3.tcl
                  -nolog -nojournal
3. If route_design's WNS isn't comfortably positive, phase 2:
                  vivado -mode batch -source run_impl_mac_array_a3_sol2_phase2_physopt.tcl
                  -nolog -nojournal
                  (rename the -f fresh solution's routed.dcp path in if not solution2/3)
4. Bitstream:     vivado -mode batch -source run_rawbin_mac_array_a3_dw_fix.tcl
                  -nolog -nojournal   (writes .bit + .bin; byte-swap the .bin
                  with a 32-bit-word reversal before deploying -- plain
                  write_bitstream -bin_file is NOT byte-swapped for this
                  board's fpgautil loader)
