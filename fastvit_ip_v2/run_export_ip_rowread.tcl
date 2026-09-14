# run_export_ip_rowread.tcl -- ZHR-92 (2026-09-14): DWR_ROWREAD (row-granularity
# read_request before the COL loop, word-packed read() inside it, via the
# already-present dw_in_burst port) on top of the deployed rowburst baseline.
# csynth probe: readreq only in the ROW body, read only in Pipeline_COL, COL
# II=1 (iter latency 12->3), zero II violations design-wide, isolated LUT
# -1,005 / DSP -4. Judgment: csim 4 suites, route_design-alone WNS >= 0
# (current +0.312), real LUT vs isolated -1,005, then board (DW 224.7 -> ?).
set proj_name  "rowread_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_ROWREAD"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROWREAD"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DDWR_ROWREAD"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

