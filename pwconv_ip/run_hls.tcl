#================================================================
# run_hls.tcl — pwconv_ip Vitis HLS 2024.2 综合脚本
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls.tcl
#================================================================

set proj_name  "pwconv_ip_proj"
set top_func   "pwconv_ip"
set part       "xc7z020clg400-1"
set clk_period "5"

open_project $proj_name
set_top $top_func

add_files pwconv_ip.cpp -cflags "-std=c++14"
add_files pwconv_ip.h   -cflags "-std=c++14"
add_files -tb tb_pwconv_ip.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation..."
csim_design -O -clean

puts ">>> Running C Synthesis..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_PWConv_IP" \
              -description  "Pointwise (1x1) Convolution for FastVIT (int8, xc7z020)" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. IP: ${proj_name}/solution1/impl/ip/"
close_project
