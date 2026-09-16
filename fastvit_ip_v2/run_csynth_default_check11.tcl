# run_csynth_default_check11.tcl -- ZHR-92 (2026-09-16): flag-less build after the
# DWR_FLAT default flip -- csynth totals + hw.h must be bit-identical to the
# board-tested flat_export build (255/22/40,327/79,843).
set proj_name  "dflt11"
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

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

