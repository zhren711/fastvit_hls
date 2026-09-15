# run_csim_dwrow_probe.tcl -- ZHR-92 (2026-09-15): csim for the DWR_ROW_PF +
# DWR_DEFER_WRESP step-1 probe on the two DW suites (raster tb 5 real shapes incl.
# fpg=2 and k=7; wiring tb 4 real shapes through the integrated top). The burst_maxi
# csim model checks request/read/write/response accounting, so a wrong deferral
# count or a wrong prefetch address would show here.
set part       "xc7z020clg400-1"
set clk_period "10"

# 1. dw_raster_layer_tb (5 real DW shapes, raster vs. original mac_array.cpp)
open_project -reset dwrow_raster_csim
set_top mac_array_top
add_files mac_array.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
add_files -tb dw_raster_layer_tb.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
open_solution "sol" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default
csim_design
close_project

# 2. wiring tb (4 real DW shapes through the integrated top)
open_project -reset dwrow_wiring_csim
set_top mac_array_top
add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
add_files -tb mac_array_raster_integrated_wiring_tb.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default
csim_design
close_project

puts ">>> both DW csim done."
exit
