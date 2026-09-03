# run_export_ip_pwchunk3.tcl -- ZHR-92 (2026-09-03): isolated csynth + export
# of the THIRD (total_iters-hoisted) fix to the chunked PW weight-loading
# regression. Fresh project name per this project's export_design
# stale-cache discipline.
set proj_name  "pwchunk3"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit
