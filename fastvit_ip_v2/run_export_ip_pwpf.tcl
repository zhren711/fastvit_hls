# run_export_ip_pwpf.tcl -- ZHR-92 (2026-09-15): PW_ROWREAD_PREFETCH (ROW_READ
# requests issued PF=8 channels ahead; read() unchanged inside ROW_READ_FILL at
# II=1; PF sized from the exported adapter: outstanding 16, request FIFO 16,
# read buffer 256 words) on top of the deployed pwdefer baseline. csynth probe:
# readreq only in ROW_READ_PRIME + the ROW_READ_CH body, read only in FILL,
# no new multiplies, zero II violations, isolated LUT +722. Judgment: six csim
# suites, route_design-alone WNS >= 0 (current +0.292), board PW 307 -> ?
# (190-220 = mechanism works; the chunked big-cin entries 60/64/66/70/72
# should be the biggest movers this time).
set proj_name  "pwpf_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_ROWREAD_PREFETCH"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_ROWREAD_PREFETCH"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_ROWREAD_PREFETCH"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

