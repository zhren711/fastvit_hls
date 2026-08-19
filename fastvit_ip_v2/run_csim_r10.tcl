#================================================================
# run_csim_r10.tcl -- Phase A round 10: correctness check after moving
# the PIPELINE annotation from each caller's own step loop into a single
# run_reduce_unified() function that both run_dwconv and run_pwconv call
# as an ordinary (unpipelined) call -- round 9 confirmed via the resource
# hierarchy that PIPELINE living in the caller forces a private instance
# per caller regardless of a shared callee.
#================================================================

set proj_name  "mac_array_poc_r10_csim"
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

puts ">>> Running C Simulation (round 10 unified-pipeline correctness check)..."
csim_design

puts ">>> Done."
exit
