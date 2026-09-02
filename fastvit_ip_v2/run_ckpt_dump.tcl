#================================================================
# run_ckpt_dump.tcl -- A2 exit measurement: run the real 82-entry
# hardware sequence against Stem's ARM-computed output + real gamma-
# folded weights, dumping 6 checkpoints for the segmented cosine table.
#
# ZHR-92 (2026-09-02): FIXED -- was linking mac_array.cpp (the OLD
# tile-based DW mechanism) since this script's own creation (2026-08-22/23,
# tools/compare_board_full_network_ckpts.py's own commit f7196f3 confirms
# tile-based DW was legitimately the deployed mechanism THAT DAY). DW
# raster integration (000350a, 2026-08-28, 5-6 days later) replaced the
# DEPLOYED mechanism but nobody updated this tcl to match -- every
# "checkpoint byte-exact" claim made after 2026-08-28 that relied on this
# script's own ckpt_hw_* reference was comparing real (raster-DW) board
# output against a stale (tile-DW) reference. Now links the same
# mac_array_raster_integrated.cpp + dw_raster_layer.cpp pair the real
# deployed bitstream is built from, matching every other current csynth/P&R
# tcl's own file list (e.g. run_csynth_baseline_10ns.tcl).
#================================================================

set proj_name  "mac_array_ckpt_dump"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb mac_array_ckpt_dump.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running A2 exit checkpoint dump..."
csim_design

puts ">>> Done."
exit
