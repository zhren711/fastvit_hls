#================================================================
# run_csynth.tcl -- Phase A mac_array PoC, C synthesis only (no P&R).
# Goal: first resource estimate (LUT/FF/BRAM/DSP) for the 8x8x8 MAC
# array geometry decision -- NOT a timing/200MHz run (Phase D, deferred).
# Reuses the same project/solution as run_csim.tcl.
#================================================================

set proj_name  "mac_array_poc"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Synthesis (resource estimate only, no P&R)..."
csynth_design

puts ">>> Done. Check mac_array_poc/solution1/syn/report/mac_array_top_csynth.rpt"
exit
