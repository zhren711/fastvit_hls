# run_export_ip_seburst.tcl -- ZHR-92 (2026-09-15): SE_BURST (GAP read-side burst +
# SCALE GELU-style burst) on top of the deployed dwrow baseline (+0.211ns). Isolated
# LUT +1,929 (datapath). Judgment: route-only WNS >= 0; real LUT; board GAP 3.75 ->
# ~0.5, SCALE 5.45 -> ~0.2, network ~295 -> 286-290 (busy-poll harness). Any surprise
# at P&R or board: drop the line (9ms is not worth a second round).
set proj_name  "seburst_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DSE_BURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DSE_BURST"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DSE_BURST"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

