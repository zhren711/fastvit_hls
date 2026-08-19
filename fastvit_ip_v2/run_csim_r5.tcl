#================================================================
# run_csim_r5.tcl -- Phase A round 5: correctness check of the fully
# restructured (true 512-physical-MAC geometry, compile-time-bounded
# reduction loops, stride-aware patch buffer) mac_array.cpp, before
# trusting its csynth resource numbers.
#================================================================

set proj_name  "mac_array_poc_r5_csim"
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

puts ">>> Running C Simulation (round 5 full rewrite correctness check)..."
csim_design

puts ">>> Done."
exit
