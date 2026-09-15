# run_export_ip_pwdefer.tcl -- ZHR-92 (2026-09-15): PW_DEFER_WRESP (PW_FLAT's
# write_response deferred to the ot two back, popped at writeout row starts;
# request+write unchanged inside the II=1 pipeline; <=8 in flight vs the
# adapter's 16) on top of the deployed rowread baseline. csynth probe:
# writereq/write inside PW_FLAT on (wr_col==3), writeresp inside on the
# disjoint (wr_col==0 & pending>=8), PW_FLAT II=1, isolated LUT +480.
# Judgment: csim 4 suites, route_design-alone WNS >= 0 (current +0.113; phys_opt
# allowed within -0.2), board PW 495 -> ? (350-380 = mechanism works).
set proj_name  "pwdefer_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_DEFER_WRESP"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_DEFER_WRESP"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_DEFER_WRESP"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

