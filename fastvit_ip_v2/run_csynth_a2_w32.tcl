#================================================================
# run_csynth_a2_w32.tcl -- A2 pre-step round 2: resource measurement at
# 32-wide array (4x4x2), with both K=7 and fpg=2 fixes in place. Checking
# LUT <=45%, DSP/FF real margin, before resuming the full A2 integration.
#================================================================

set proj_name  "mac_array_poc_a2_w32"
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
