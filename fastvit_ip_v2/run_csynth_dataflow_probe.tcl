# run_csynth_dataflow_probe.tcl -- ZHR-92, 2026-08-23: DATAFLOW feasibility
# probe, isolated from mac_array_top's own project/solution entirely.
set proj_name  "dataflow_probe_proj"
set top_func   "dataflow_probe_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files dataflow_probe.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/${top_func}_csynth.rpt"
exit
