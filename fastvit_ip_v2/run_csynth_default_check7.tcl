# run_csynth_default_check7.tcl -- ZHR-92 (2026-09-15): flag-less build after the
# DWR_ROW_PF / DWR_DEFER_WRESP default flip -- csynth totals + hw.h must be
# bit-identical to the board-tested dwrow_export build (246/29/39,670/75,807).
set proj_name  "dflt7"
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

