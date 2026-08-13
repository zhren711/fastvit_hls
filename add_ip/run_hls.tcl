#============================================================
# run_hls.tcl
# add_ip Vitis HLS 综合脚本
#============================================================

# 打开/创建项目
# 用法: vitis_hls.bat -f run_hls.tcl -nolog
# (-nolog 绕过 Windows 上 tee.exe 缺失问题)

open_project add_ip_proj

# 设置顶层函数
set_top add_ip

# 添加源文件
add_files add_ip.cpp

# 添加 testbench
add_files -tb tb_add_ip.cpp

# 打开解决方案 (如果不存在则创建)
open_solution -reset solution1

# 设置目标器件
set_part {xc7z020clg400-1}

# 设置时钟周期 (100MHz -> 10ns)
create_clock -period 5 -name default

# 配置
config_compile -name_max_length 256
config_rtl -reset control

# 运行 C 仿真
csim_design -clean

# 运行 C 综合
csynth_design

# 导出 RTL (可选, 取消注释以启用)
# export_design -format ip_catalog -vendor "user_org" -version "1.0"

# 退出
exit
