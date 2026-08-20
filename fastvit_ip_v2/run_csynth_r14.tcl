#================================================================
# run_csynth_r14.tcl -- Phase A round 14: csynth after extracting
# drive_mac() as its own non-inlined, function-pipelined unit called from
# two ordinary (non-PIPELINE) loops. Decisive check first: does
# run_reduce_unified's DSP come back to 512, or duplicate to 1024 again.
#================================================================

set proj_name  "mac_array_poc_r14"
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
