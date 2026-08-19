#================================================================
# run_csynth_r7.tcl -- Phase A round 7: csynth after giving PW's REDUCE
# per-dd accumulators (mirrors DW's acc[MAC_PD][MAC_PR][MAC_PC]). Checking:
# does run_pwconv's DSP return to ~520 (mac_muladd/dsp_slice binding
# restored), LUT return to ~41k, and REDUCE still report II=1.
#================================================================

set proj_name  "mac_array_poc_r7"
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
