#================================================================
# run_hls_200mhz.tcl — dwconv_ip, 200MHz (5ns), DSP binding
# 用法: vitis_hls -f run_hls_200mhz.tcl
#================================================================

set proj_name "dwconv_ip_proj"
set top_func  "dwconv_ip"
set part      "xc7z020clg400-1"

open_project $proj_name
set_top $top_func

add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"

open_solution "solution1" -reset -flow_target vivado
set_part $part
create_clock -period 5 -name default

puts ">>> C Synthesis (200MHz, DSP binding)..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP_200MHz" \
              -description  "Depthwise Conv, int8, K=3/7, DSP-bound, 200MHz" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done."
close_project
