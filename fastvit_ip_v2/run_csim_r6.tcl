#================================================================
# run_csim_r6.tcl -- Phase A round 6: correctness check of the PW REDUCE
# adder-tree rewrite (fixes the II=8 carried-dependency stall on acc[rr][cw]
# found in round 5). Single variable vs round 5: only run_pwconv's REDUCE
# loop body changed (chained += -> independent psum[] + balanced tree).
#================================================================

set proj_name  "mac_array_poc_r6_csim"
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

puts ">>> Running C Simulation (round 6 PW REDUCE adder-tree correctness check)..."
csim_design

puts ">>> Done."
exit
