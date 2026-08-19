#================================================================
# run_csynth_r8.tcl -- Phase A round 8: csynth after moving PW's last-
# channel-tile boundary guard out of the unrolled REDUCE region (into a
# staging-time zero-fill of wtile). Checking: PW LUT <=45k, DSP still
# ~524, REDUCE still II=1.
#================================================================

set proj_name  "mac_array_poc_r8"
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
