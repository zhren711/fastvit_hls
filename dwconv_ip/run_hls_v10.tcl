#================================================================
# run_hls_v10.tcl — dwconv_ip v10.0 (channel-stationary)
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls_v10.tcl
#================================================================

set proj_name  "dwconv_ip_proj"
set top_func   "dwconv_ip"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Synthesis (channel-stationary v10.0)..."
csynth_design

puts ">>> Exporting IP v10.0..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP" \
              -description  "DW Conv v10 channel-stationary, K=3/7, xc7z020" \
              -vendor        "user.org" \
              -version       "10.0"

puts ">>> Done. Check solution1/syn/report/dwconv_ip_csynth.rpt for resource usage."
close_project
