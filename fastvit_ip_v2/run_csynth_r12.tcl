#================================================================
# run_csynth_r12.tcl -- Phase A round 12: csynth after specializing dw_S
# to a compile-time literal in run_reduce_unified's DW gather. Checking:
# run_reduce_unified LUT <=35k, top-level LUT <=70k, DSP still 535 (not
# 1024 -- MAC array must not have been duplicated by the gather branch),
# UNIFIED still II=1.
#================================================================

set proj_name  "mac_array_poc_r12"
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
