#================================================================
# run_csynth_r13.tcl -- Phase A round 13: csynth after giving DW its own
# compile-time-bounded (kh,kw) nested loop instead of deriving kh/kw from
# a shared runtime step counter. First and decisive check: does DW's now-
# textually-separate LANE_D/R/C block duplicate the MAC array (DSP
# 512->1024), or does it still share with PW's?
#================================================================

set proj_name  "mac_array_poc_r13"
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
