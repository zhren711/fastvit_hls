# run_csynth_a3_sol32_burstwriteout.tcl -- ZHR-92 angle-B step (2026-08-24):
# hls::burst_maxi integration for WRITEOUT only, out_burst on the SAME
# bundle=gmem_act as out_base (not a 5th master). csynth-only.
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution32" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution32/syn/report/mac_array_top_csynth.rpt"
exit
