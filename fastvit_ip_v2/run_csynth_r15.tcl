#================================================================
# run_csynth_r15.tcl -- Phase A round 15: csynth at the reduced 4x8x2=64
# array width (deliberate registered reproduction deviation, ZHR-64).
# Checking: LUT/FF/DSP all inside budget (53.2k/106.4k/220).
#================================================================

set proj_name  "mac_array_poc_r15"
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
