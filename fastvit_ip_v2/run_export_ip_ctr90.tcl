# run_export_ip_ctr90.tcl -- ZHR-92 (2026-09-16): CTR_NARROW (the four loop-carried counter
# chains from the frequency sweep narrowed/folded; step-1 estimates 7.60/7.78/7.30/7.30 <= 8.5)
# on top of the deployed dwflat source, HLS create_clock at 9.0ns. Vivado side: run_impl_ctr90.tcl.
set proj_name  "ctr90_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "9.0"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DCTR_NARROW"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DCTR_NARROW"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DCTR_NARROW"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

