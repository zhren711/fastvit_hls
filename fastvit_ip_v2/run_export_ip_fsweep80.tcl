# run_export_ip_fsweep80.tcl -- ZHR-92 (2026-09-16): frequency sweep point 8.0ns
# (125MHz) on the deployed dwflat source (flag-less), HLS create_clock retargeted;
# the Vivado side is retargeted in run_impl_fsweep80.tcl. P&R only, no board.
set proj_name  "fsweep80_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "8.0"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

