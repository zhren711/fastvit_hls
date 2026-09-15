# run_export_ip_whoist.tcl -- ZHR-92 (2026-09-15): PW_WHOIST_WIDE (32-bit burst_maxi
# w_burst on gmem_w feeding PW_WEIGHT_HOIST one word/cycle) on top of the deployed
# seburst baseline (+0.254ns). Isolated LUT +322 (control-only: exception case 2).
# Judgment: route-only WNS >= 0; real LUT; board PW 181 -> 165-171 (as modeled) /
# 175-179 (cache write side binding) / >=181 (weight term not as fitted).
# NEW REGISTER 0x10c/0x110 -- only the *_whoist ARM binaries work with this build.
set proj_name  "whoist_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

