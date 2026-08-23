# run_csynth_a3_probe.tcl -- same as run_csynth_a3_axi.tcl but a separate
# solution name (solution14) to sidestep an external file lock on
# mac_array_poc_a3_axi/solution1/syn/report (2026-08-22, ZHR-92 round 4
# probe -- pw_patch_full partition-pragma resource test).
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution14" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution14/syn/report/mac_array_top_csynth.rpt"
exit
