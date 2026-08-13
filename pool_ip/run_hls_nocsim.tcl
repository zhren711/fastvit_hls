# run_hls_nocsim.tcl — pool_ip 跳过 csim（v2 GlobalAvgPool-only）
set proj_name  "pool_ip_proj"
set part       "xc7z020clg400-1"
set clk_period "5"

open_project $proj_name
set_top "pool_ip"
add_files pool_ip.cpp -cflags "-std=c++14"
add_files pool_ip.h   -cflags "-std=c++14"
open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Synthesis (no csim)..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_Pool_IP" \
              -description  "GlobalAvgPool for FastVIT SE block (int8 output, xc7z020)" \
              -vendor "user.org" -version "1.0"

puts ">>> Done."
close_project
