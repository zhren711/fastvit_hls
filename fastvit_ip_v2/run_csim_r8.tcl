#================================================================
# run_csim_r8.tcl -- Phase A round 8: correctness check of moving PW's
# last-channel-tile boundary guard out of the unrolled REDUCE region and
# into WSTAGE (zero-pad wtile to MAX_CIN_PW instead of `if (dd>=chunk_sz)
# continue` inside LANE_D). Single variable vs round 7: only WSTAGE's loop
# bound/body and REDUCE's guard changed.
#================================================================

set proj_name  "mac_array_poc_r8_csim"
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

puts ">>> Running C Simulation (round 8 PW staging-zero-fill correctness check)..."
csim_design

puts ">>> Done."
exit
