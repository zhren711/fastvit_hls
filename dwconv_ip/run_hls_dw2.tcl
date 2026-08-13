#================================================================
# run_hls_dw2.tcl — dwconv_ip DW_TN=2, version 2.0
# 强制 Vivado 重新综合 (版本 2.0 ≠ 1.0)
#================================================================

set proj_name  "dwconv_ip_proj"
set top_func   "dwconv_ip"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"

open_solution "solution1" -reset -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> C Synthesis (DW_TN=2)..."
csynth_design

puts ">>> Exporting IP as version 2.0 (triggers Vivado re-synthesis)..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP_v2" \
              -description  "Depthwise Conv, DW_TN=2, 7ns" \
              -vendor        "user.org" \
              -version       "2.0"

puts ">>> Done."
close_project
