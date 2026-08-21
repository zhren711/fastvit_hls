#================================================================
# run_csynth_a1_final.tcl -- A1 exit checkpoint: csynth with all operators
# (DW, PW, Add, GAP, ReLU, Sigmoid, Scale, GELU) present. First real
# measurement of whether round 15's 31k LUT / 137 DSP / 80k FF margin
# (DW+PW+toy driver only) is enough for the full A1 operator set.
#================================================================

set proj_name  "mac_array_poc_a1_final"
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
