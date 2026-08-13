# export_only.tcl - 仅导出 IP，跳过 csim/csynth
# 用法: vitis_hls.bat -f export_only.tcl -nolog

open_project add_ip_proj
set_top add_ip
add_files add_ip.cpp

open_solution "solution1"
set_part {xc7z020clg400-1}
create_clock -period 10 -name default

export_design -format ip_catalog \
              -display_name "FastVIT_Add_IP" \
              -description  "ElementWise Add for FastVIT (int8, xc7z020)" \
              -vendor        "user.org" \
              -version       "1.0"

exit
