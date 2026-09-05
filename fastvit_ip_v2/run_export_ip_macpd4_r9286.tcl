# run_export_ip_macpd4_r9286.tcl -- ZHR-92 margin-search round, point 1/2.
# 9.375ns HLS create_clock, matching PS7's REAL achievable FCLK0 (requested
# config value 108 snaps to actual 107.692307MHz = 9.2857ns period, per
# check_ps7_freq_105108.tcl's own tool-computed readback -- the nominal
# 105MHz/9.5ns target is NOT an achievable Zynq-7000 IO PLL divider value).
set proj_name  "macpd4_r9286"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "9.2857"

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
