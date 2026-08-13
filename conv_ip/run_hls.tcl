#================================================================
# run_hls.tcl — conv_ip Vitis HLS 2024.2 综合脚本
# 目标: MicroZed xc7z020clg400-1
# 功能: 标准卷积 (Standard Conv, K=3/7, group=1)
# 注意: DW Conv → dwconv_ip, PW Conv → pwconv_ip (独立IP)
# 用法: vitis_hls -f run_hls.tcl
#================================================================

set proj_name  "conv_ip_proj"
set top_func   "conv_ip"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files conv_ip.cpp -cflags "-std=c++14"
add_files conv_ip.h   -cflags "-std=c++14"
add_files -tb conv_ip_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation..."
csim_design -O -clean

puts ">>> Running C Synthesis..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_Conv_IP" \
              -description  "Standard Convolution for FastVIT (int8, K=3/7, xc7z020)" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. IP: ${proj_name}/solution1/impl/ip/"
close_project
