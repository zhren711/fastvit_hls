# run_csynth_a3_sol19_check.tcl -- csynth for the PW_PATCH_HOIST merge round
# (2026-08-23, ZHR-92): checks DSP count and whether gmem_act_wide disappears
# from the interface report now that in_base_wide shares bundle=gmem_act.
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution19" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution19/syn/report/mac_array_top_csynth.rpt"
exit
