#================================================================
# run_export_ip_dwraster_step2_lut.tcl -- ZHR-92, 2026-08-28, DW raster
# integration (Step 2) P&R export, LUT-mode binding (no LB_FORCE_DSP).
# Fresh project name (mp3, not mp2) since add_files cflags are
# project-scoped in Vitis HLS, and mp2 already has dw_raster_layer.cpp
# registered with -DLB_FORCE_DSP from the forced-DSP round.
#================================================================

set proj_name  "mp3"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
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
