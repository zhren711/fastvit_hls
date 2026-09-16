# run_export_ip_dwwb.tcl -- ZHR-92 (2026-09-15): PW_WHOIST_WIDE + DWR_WBURST (option (a):
# the DW prologue reads kernel+shift through the same 32-bit w_burst port, restoring
# what the wide port took from w_base) on top of the deployed seburst baseline.
# Judgment: route-only WNS >= 0 (whoist rolled +0.088); board DW 83.6 -> ~66
# (restored) / <60 (prologue cheaper too) / >72 (word loop worse than the old burst);
# PW 160.6 stays; network 269 -> ~248-252. *_whoist ARM binaries (register 0x10c).
set proj_name  "dwwb_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE -DDWR_WBURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE -DDWR_WBURST"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE -DDWR_WBURST"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

