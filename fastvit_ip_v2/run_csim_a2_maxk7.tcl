#================================================================
# run_csim_a2_maxk7.tcl -- A2 pre-step: MAX_K 3->7 (real network has 13
# DW layers at K=7, silently truncated to 9/49 taps before this fix).
# Runs the FULL existing suite too, since MAX_K is a single global bound
# shared by every DW call -- must confirm the K=3 layers still work
# correctly at the new bound, not just that K=7 is now correct.
#================================================================

set proj_name  "mac_array_poc_a2_maxk7_csim"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation (A2 pre-step: MAX_K=7)..."
csim_design

puts ">>> Done."
exit
