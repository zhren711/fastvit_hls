# run_export_ip_dwob2.tcl -- ZHR-92 (2026-09-13): DW_OUTPUT_BURST re-test
# (port-reuse form,) on top of SHARED_MUL_ARMS -- the
# shared 32x32 multiplier sink that killed both prior forms (-0.418/-0.461)
# no longer exists. Judgment, pre-registered: (1) csynth LUT (real-P&R
# expectation ~44,300 from this mechanism's two prior real deltas, -280 and
# -334; stop and re-diagnose as a RESOURCE problem if real lands >85%) and
# dwr_consume achieved II still 2; (2) csim 4 suites; (3) route_design-alone
# WNS >= 0. If (3) fails and the critical path is one of the route-dominated
# population (DW producer chain / descriptor RAM / gmem_w->DW gather /
# DW->store FIFO), the conclusion is "DW's II win is unreachable at this
# placement density" and the line closes.
set proj_name  "merge4_probe"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_ROWREAD_MERGE4"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_ROWREAD_MERGE4"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_ROWREAD_MERGE4"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

