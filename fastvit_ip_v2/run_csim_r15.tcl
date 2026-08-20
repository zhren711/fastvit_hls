#================================================================
# run_csim_r15.tcl -- Phase A round 15: correctness check after dropping
# MAC_PR/MAC_PC/MAC_PD from 8x8x8=512 to 4x8x2=64 (deliberate registered
# reproduction deviation, ZHR-64). Two bugs caught and fixed while making
# this change: (1) PW's WRITEOUT combine tree was hardcoded to 8 terms
# (acc[0]..acc[7]), now a generic MAC_PD-wide unrolled sum; (2) all four
# `cyclic factor=8` ARRAY_PARTITION pragmas were hardcoded to the old
# MAC_PD, now `factor=MAC_PD` -- with the literal 8 left in place, PW's
# bank=(cib+dd) mod 8 would stop being compile-time-known for unrolled dd.
#================================================================

set proj_name  "mac_array_poc_r15_csim"
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

puts ">>> Running C Simulation (round 15 64-wide-array correctness check)..."
csim_design

puts ">>> Done."
exit
