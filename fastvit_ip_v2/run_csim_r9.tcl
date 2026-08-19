#================================================================
# run_csim_r9.tcl -- Phase A round 9: correctness check of merging DW and
# PW onto one shared mac_reduce_step() (INLINE off) instead of each having
# its own private 512-MAC REDUCE loop. Testbench extended with Phase 3
# (PW->DW transition) and Phase 4 (DW->PW(32)->PW(8) mixed sequence) to
# cover directions/orderings round 5-8's DW-then-PW-only test never
# exercised.
#================================================================

set proj_name  "mac_array_poc_r9_csim"
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

puts ">>> Running C Simulation (round 9 shared-MAC-array correctness check)..."
csim_design

puts ">>> Done."
exit
