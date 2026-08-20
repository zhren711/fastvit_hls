#================================================================
# run_csim_r12.tcl -- Phase A round 12: correctness check after
# specializing dw_S to a compile-time literal (1 or 2) inside
# run_reduce_unified's DW gather branch, to collapse the per-lane
# runtime address range from 6 (2 S-values x 3 kh-values) to 3 per axis.
# PW branch untouched. MAC array itself untouched -- only which gather
# branch populates lane_in/lane_w changes.
#================================================================

set proj_name  "mac_array_poc_r12_csim"
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

puts ">>> Running C Simulation (round 12 dw_S-specialized-gather correctness check)..."
csim_design

puts ">>> Done."
exit
