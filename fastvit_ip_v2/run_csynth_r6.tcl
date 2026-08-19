#================================================================
# run_csynth_r6.tcl -- Phase A round 6: csynth after the PW REDUCE
# adder-tree rewrite. Single variable vs round 5's csynth (mac_array_poc_r5):
# only run_pwconv's REDUCE loop body changed. Checking: does REDUCE now
# report II=1, and do DSP/LUT stay near round 5's 1047/188,461 (a big move
# would mean the tree introduced something else, not just fixing II).
#================================================================

set proj_name  "mac_array_poc_r6"
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
