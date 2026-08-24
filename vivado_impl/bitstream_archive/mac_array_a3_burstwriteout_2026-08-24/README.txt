A3 mac_array_top bitstream -- WRITEOUT hls::burst_maxi dual-path, X0-120 pblock
================================================================================

Archived 2026-08-24. See ZHR-92 (Linear) for the full round-by-round writeup.
Closes the WRITEOUT-burst investigation line: 6 rounds of inference-based burst
attempts (all failed, AccessInCondBranchMissed/CouldNotAnalyzePatternMissed),
then memcpy (failed, same conditional-branch rejection), then config_interface
tuning (widen/alignment/latency/burst-length, no effect), then explicit
hls::burst_maxi -- which bypasses the inference analyzer entirely and, after
one crashed attempt (8-bit view on an already-32-bit-widened bundle) and one
functionally-buggy attempt (missing byte-alignment check, caught by csim, not
board), landed here: clean, verified, board-measured real gain.

What changed
------------
PW's WRITEOUT (inside pw_flat_pipeline) now has two paths sharing bundle=
gmem_act (NOT a 5th master):
  - Fast path: hls::burst_maxi<ap_uint<32>> out_burst, used when a row is both
    full (col_sz==MAC_PC) AND its byte address is 4-aligned. One row (MAC_PC=4
    act_t) packs into one 32-bit word, single-word burst_maxi write per row.
  - Slow path: plain out_base[] store, used otherwise (partial columns, or a
    misaligned row start -- both real conditions, not just synthetic ones;
    only entry76/78, the SE block's w_out=1 fc1/fc2 layers, exercise it on the
    real 82-entry network, and they fail col_sz==MAC_PC on their own regardless
    of alignment).
Real network cost of the alignment check: zero -- every other real PW layer's
w_out is a multiple of MAC_PC (128/64/32/16/8/4), so the alignment condition
never flips the verdict col_sz==MAC_PC alone would already give.

Also: LayerDescV2's use_wide_path field marked formally DEPRECATED (dead,
confirmed via grep across all tools/*.py generators and mac_array.cpp -- kept
declared, zero-init-safe, not deleted to avoid a descriptor-format change).

Source
------
fastvit_ip_v2/mac_array.cpp: pw_flat_pipeline's WRITEOUT, mac_array_top's
out_burst parameter (bundle=gmem_act, s_axilite bundle=control).
fastvit_ip_v2/mac_array.h: mac_array_top prototype updated (9 params), adds
#include <hls_burst_maxi.h>, use_wide_path deprecation note.
fastvit_ip_v2/mac_array_tb.cpp: all 18 mac_array_top call sites pass a
same-buffer hls::burst_maxi<ap_uint<32>> view via reinterpret_cast.
fastvit_ip_v2/a3_single_op_test_src/mac_array_driver.h +
mac_array_single_op_test.c: ARM driver writes MAC_OUT_BURST_LO/HI (offset
0x6c/0x70, read from solution35's own generated xmac_array_top_hw.h, not
guessed) to the same physical address as MAC_OUT_BASE_LO/HI.

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution35.
Vivado: vivado_impl/run_impl_mac_array_a3_sol35_burstwriteout.tcl, with
pblock_mac_array_pre_place_wide.tcl (X0-120, same as the last successful
PW-flat round -- LUT only grew +1.1% from that round's csynth estimate,
well under the scale that has previously required a pblock resize).

Files in this directory
------------------------
mac_array_bd_wrapper_burstwriteout.bit          -- Vivado bitstream (not
                                                     byte-swapped)
mac_array_bd_wrapper_burstwriteout.bin          -- Vivado's own -bin_file
                                                     output -- NOT board-
                                                     loadable directly, see
                                                     gotcha below
mac_array_bd_wrapper_burstwriteout_swapped.bin  -- byte-swapped .bin, board-
                                                     loadable (pulled directly
                                                     from the board's
                                                     /lib/firmware after load,
                                                     not locally re-derived)

Gotcha hit this round, worth flagging for next time: Vivado's own
`write_bitstream -bin_file` output is NOT the byte-swapped format the Zynq-7000
devcfg FPGA manager driver requires -- loading it directly failed with
"Invalid bitstream, could not find a sync word. Bitstream must be a byte
swapped .bin file" (confirmed via dmesg, not guessed). The actual required
conversion is fpga_overlay.py's own bit_to_bin() function (word-swaps the .bit
file's payload), which is what every previously-archived bitstream's
"_swapped.bin" file actually went through -- Vivado's -bin_file flag output
was never the deployable artifact, just an intermediate one this round
initially (and wrongly) assumed was ready to load.

Deployed to board as /lib/firmware/mac_array_bd_wrapper_burstwriteout.bin (md5
b32e4b314d5b2cd61cb0d6c6d786536a, the swapped version). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
route_design alone (no phys_opt_design needed): WNS +0.098407ns.
LUT: 30,873/53,200 (58.03%) -- real P&R came in far under the csynth
estimate's 52,646/53,200 (98.96%), consistent with this project's own
established "real P&R beats the csynth estimate by ~25 points" pattern.
DSP: 63/220 (28.64%). BRAM: 48/280 (17.14%).

Board verification -- REAL IMPROVEMENT, confirmed
----------------------------------------------------
entry3 (PW, cin=cout=48, 64x64, real network shape): 0/196,608 mismatches,
byte-exact, 86.29ms. Down from 107.82ms (the PW-flat-only baseline,
solution26/a3wp24) -- a real, board-measured 19.97% reduction.

entry5_dw (DW, cin=cout=48, 64x64): 0/196,608 mismatches, byte-exact,
70.09ms -- matches the established 70.04-70.08ms baseline, no regression
(DW's own WRITEOUT_DW was untouched this round, still plain out_base).

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_csynth_a3_sol35_burstwriteout_final.tcl
   (or run_export_ip_a3_sol35.tcl for a fresh IP export into a new solution)
2. vivado_impl:   vivado -mode batch -source
                  run_impl_mac_array_a3_sol35_burstwriteout.tcl -nolog -nojournal
3. Bitstream:     vivado -mode batch -source
                  run_bitstream_mac_array_a3_sol35_burstwriteout.tcl -nolog -nojournal
4. Board:         convert via fpga_overlay.py's bit_to_bin(), NOT Vivado's own
                  -bin_file output (see gotcha above)
