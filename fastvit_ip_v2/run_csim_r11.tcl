#================================================================
# run_csim_r11.tcl -- Phase A round 11: correctness check after removing
# run_dwconv/run_pwconv entirely -- one run_layer() function, one (rt,
# colt,ot) tile loop nest, one call site into run_reduce_unified,
# mac_array_top calls run_layer unconditionally (no more op_type dispatch
# at that level). Third and last attempt at DSP-sharing along this route.
#================================================================

set proj_name  "mac_array_poc_r11_csim"
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

puts ">>> Running C Simulation (round 11 single-function-single-call-site correctness check)..."
csim_design

puts ">>> Done."
exit
