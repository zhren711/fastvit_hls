# run_csynth_a3_sol35_burstwriteout_final.tcl -- ZHR-92 angle-B final
# (2026-08-24): dual-path WRITEOUT (burst fast path, out_base slow path),
# alignment-aware fast-path eligibility. csim already 16/16 (solution34).
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution35" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution35/syn/report/mac_array_top_csynth.rpt"
exit
