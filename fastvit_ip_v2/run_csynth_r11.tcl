#================================================================
# run_csynth_r11.tcl -- Phase A round 11: csynth after removing run_dwconv
# /run_pwconv entirely (single run_layer function, single call site into
# run_reduce_unified, mac_array_top dispatch removed). Third and last
# attempt at DSP-sharing along this route -- decisive check: DSP ~524 or
# stop and re-evaluate the whole approach.
#================================================================

set proj_name  "mac_array_poc_r11"
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
