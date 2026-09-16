# run_export_ip_keep3_100.tcl -- ZHR-92 (2026-09-16): flag-less build (CTR_NARROW + PW_MAC_PREG + PW_ACC_NARROW
# ON by default) with PW_DEFER_WRESP_KEEP_OTS=3 (pop the ot three back, <= 12 in flight). Promotion candidate.
set proj_name  "keep3_100_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

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

