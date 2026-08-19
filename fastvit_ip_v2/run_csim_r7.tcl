#================================================================
# run_csim_r7.tcl -- Phase A round 7: correctness check of giving PW's
# REDUCE loop per-dd accumulators (acc[MAC_PD][MAC_PR][MAC_PC], mirroring
# DW), replacing round 6's shared acc[rr][cw] + separate psum[] tree.
# Single variable vs round 6: only run_pwconv's acc declaration/RESET/
# REDUCE/WRITEOUT changed.
#================================================================

set proj_name  "mac_array_poc_r7_csim"
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

puts ">>> Running C Simulation (round 7 PW per-dd-accumulator correctness check)..."
csim_design

puts ">>> Done."
exit
