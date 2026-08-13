#================================================================
# run_hls.tcl — pool_ip Vitis HLS 2024.2 综合脚本
# 目标板: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls.tcl
#================================================================

set proj_name  "pool_ip_proj"
set top_func   "pool_ip"
set part       "xc7z020clg400-1"
set clk_period "5"

open_project $proj_name
set_top $top_func

add_files pool_ip.cpp -cflags "-std=c++14"
add_files pool_ip.h   -cflags "-std=c++14"
add_files -tb pool_ip_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation..."
csim_design -O -clean

puts ">>> Running C Synthesis..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_Pool_IP" \
              -description  "MaxPool/AvgPool/GlobalAvgPool for FastVIT (int8, xc7z020)" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. IP exported to ${proj_name}/solution1/impl/ip/"
close_project

#================================================================
# global_avgpool_ip 单独打包 (SE块专用)
#================================================================
open_project "gap_ip_proj"
set_top "global_avgpool_ip"

add_files pool_ip.cpp -cflags "-std=c++14"
add_files pool_ip.h   -cflags "-std=c++14"
add_files -tb pool_ip_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design

export_design -format ip_catalog \
              -display_name "FastVIT_GlobalAvgPool_IP" \
              -description  "Global Average Pool for SE block (int32 output)" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> GlobalAvgPool IP exported."
close_project
