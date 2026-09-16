# run_export_ip_plane8.tcl -- ZHR-92 (2026-09-15): PW_ROWREAD_PLANE8 (W=8 layers read the
# whole Cin*64B block per rt, ~100 requests instead of 18,048) + DWR_WBURST_PF (both
# lanes' requests up front) on top of the deployed wburst baseline (+0.180ns, 87.15% LUT).
# Isolated LUT +1,954. Judgment: route-only WNS >= 0 (first gate: real LUT); board PW
# 160.6 -> 153-156 (as modeled) / 158-160 (2x data ate it: gate off); DW 67.6 -> ~66.5;
# network 253 -> ~246-247. *_whoist ARM binaries (register map unchanged).
set proj_name  "plane8_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_ROWREAD_PLANE8 -DDWR_WBURST_PF"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_ROWREAD_PLANE8 -DDWR_WBURST_PF"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_ROWREAD_PLANE8 -DDWR_WBURST_PF"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

