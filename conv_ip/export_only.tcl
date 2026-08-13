# export_only.tcl - 仅导出 IP，跳过 csim/csynth
# 用法: vitis_hls.bat -f export_only.tcl -nolog

open_project conv_ip_proj
set_top conv_ip
add_files conv_ip.cpp -cflags "-std=c++14"
add_files conv_ip.h   -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part {xc7z020clg400-1}
create_clock -period 10 -name default

export_design -format ip_catalog \
              -display_name "FastVIT_Conv_IP" \
              -description  "Standard Convolution for FastVIT (int8, TN=2 TM=2, xc7z020)" \
              -vendor        "user.org" \
              -version       "1.0"

exit
