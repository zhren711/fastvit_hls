#================================================================
# run_csynth_r10.tcl -- Phase A round 10: csynth after moving PIPELINE
# into run_reduce_unified itself (single unified step loop), with
# run_dwconv/run_pwconv reduced to one ordinary call each. Checking, in
# order: 1) run_reduce_unified appears once in the resource hierarchy,
# 2) DSP drops to ~524, 3) LUT in 180-200k, 4) UNIFIED still reports II=1.
#================================================================

set proj_name  "mac_array_poc_r10"
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

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/mac_array_top_csynth.rpt"
exit
