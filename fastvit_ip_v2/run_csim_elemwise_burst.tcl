# run_csim_elemwise_burst.tcl -- ZHR-92 (2026-09-06): csim for the
# ELEMWISE_BURST rewrite of run_gelu/run_add. New dedicated testbench
# (gelu_add_burst_tb.cpp) since no existing testbench in this codebase
# exercises GELU/ADD against the current raster architecture.
set proj_name  "elemwise_burst_csim"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csim_design
exit
