# run_csynth_burst_maxi_probe.tcl -- ZHR-92 angle-B step 2 (2026-08-24):
# isolated hls::burst_maxi availability probe under -flow_target vivado
# (this project's real IP-export flow). csynth only.
set proj_name  "burst_maxi_probe_proj"
set top_func   "burst_maxi_probe_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files burst_maxi_probe/burst_maxi_probe.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/${top_func}_csynth.rpt"
exit
