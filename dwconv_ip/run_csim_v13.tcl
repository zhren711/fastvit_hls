#================================================================
# run_csim_v13.tcl — csim-only correctness check for v13 DSP packing
# 用法: vitis_hls -f run_csim_v13.tcl
# 只做 C 仿真 (不做 csynth/cosim)，验证 dsp_packed_mac2 打包数学是否正确
#================================================================

set proj_name  "dwconv_ip_csim_v13_proj"
set top_func   "dwconv_ip"

open_project $proj_name
set_top $top_func

add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"
add_files -tb tb_csim_v13.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 7 -name default

puts ">>> Running C Simulation (v13 DSP-packed MAC correctness check)..."
csim_design

close_project
