#================================================================
# run_csynth_r9.tcl -- Phase A round 9: csynth after merging DW and PW
# onto one shared mac_reduce_step() (INLINE off). Checking, in order:
# 1) the module hierarchy shows ONE mac_reduce_step instance, not two
# 2) DSP drops from 1047 to ~524
# 3) LUT lands in the 180-200k range (flat vs round 8's 182,571 -- this is
#    the pre-registered expectation, not a target to beat)
#================================================================

set proj_name  "mac_array_poc_r9"
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
