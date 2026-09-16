# run_export_ip_preg90.tcl -- ZHR-92 (2026-09-16): layer (2) of the 111MHz line on top of CTR_NARROW --
# PW_MAC_PREG (product registered, mul_8s_8s_16_2_1; PW_FLAT II=1, depth 9 -> 11) + PW_ACC_NARROW
# (ap_int<26> lane accumulators). Isolated 255/22/40,847/78,124 (CTR_NARROW 255/22/41,200/79,206).
set proj_name  "preg90_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "9.0"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

