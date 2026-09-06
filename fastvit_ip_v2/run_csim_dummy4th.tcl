# run_csim_dummy4th.tcl -- ZHR-92 (2026-09-05): csim sanity check for the
# DUMMY_FOURTH_MASTER diagnostic build before spending isolated csynth/P&R.
set proj_name  "dummy4thcsim"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb mac_array_raster_integrated_wiring_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csim_design
exit
