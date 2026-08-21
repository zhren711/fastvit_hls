#================================================================
# run_csynth_a2_maxk7.tcl -- A2 pre-step: resource impact of MAX_K 3->7
# (patch buffer grows, DW tap trip count grows 9->49). Checking whether
# this alone pushes the design back out of budget before any of the 41
# additional A2 descriptor-fold ops are added.
#================================================================

set proj_name  "mac_array_poc_a2_maxk7"
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
