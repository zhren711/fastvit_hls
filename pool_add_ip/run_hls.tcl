# run_hls.tcl - pool_add_ip HLS synthesis script
# 用法: vitis_hls.bat -f run_hls.tcl -nolog

# 项目设置
open_project pool_add_ip_proj
set_top pool_add_ip
add_files pool_add_ip.cpp
add_files pool_add_ip.h
add_files -tb tb_pool_add_ip.cpp

# 目标器件: MicroZed xc7z020clg400-1, 100MHz
open_solution "solution1" -flow_target vivado
set_part {xc7z020clg400-1}
create_clock -period 10 -name default

# C 仿真 (验证逻辑正确性)
csim_design

# HLS 综合
csynth_design

# 导出 IP (可选, 综合通过后再启用)
# export_design -format ip_catalog

exit
