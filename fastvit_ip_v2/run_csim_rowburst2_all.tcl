# run_csim_dwob2_all.tcl -- ZHR-92 (2026-09-13): DW_OUTPUT_BURST re-test on SHARED_MUL_ARMS 
# round, csim regression on all three real testbenches in one session
# (DW_OUTPUT_BURST gated OFF -> default DW path is source-identical to the
# deployed baseline; the only functional change under test is the hoist
# of hw/total out of the six scalar ops into mac_array_top).
set part       "xc7z020clg400-1"
set clk_period "10"

# 1. dw_raster_layer_tb (5 real DW shapes, raster vs. original mac_array.cpp)
open_project -reset rb2_raster_csim
set_top mac_array_top
add_files mac_array.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files -tb dw_raster_layer_tb.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
open_solution "sol" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default
csim_design
close_project

# 2. wiring tb (4 real DW shapes through the integrated top)
open_project -reset rb2_wiring_csim
set_top mac_array_top
add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files -tb mac_array_raster_integrated_wiring_tb.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default
csim_design
close_project

# 3. gelu_add_burst_tb (8 GELU/ADD cases -- the ops whose size computation
#    this round actually moved; the most direct correctness check here)
open_project -reset rb2_elemwise_csim
set_top mac_array_top
add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default
csim_design
close_project
puts ">>> all three csim done."
exit
