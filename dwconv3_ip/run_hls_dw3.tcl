#================================================================
# run_hls_dw3.tcl — dwconv3_ip v1.0 综合脚本
#   K=3 专用 DW Conv, TN=4 并行
#   目标: xc7z020clg400-1, 7ns (142MHz, 留时序余量)
#
# 用法: vitis_hls -f run_hls_dw3.tcl
#================================================================

set proj_name "dwconv3_ip_proj"
set top_func  "dwconv3_ip"
set part      "xc7z020clg400-1"

open_project $proj_name
set_top $top_func

add_files dwconv3_ip.cpp -cflags "-std=c++14"
add_files dwconv3_ip.h   -cflags "-std=c++14"

open_solution "solution1" -reset -flow_target vivado
set_part $part
create_clock -period 7 -name default

puts ">>> C Synthesis (dwconv3_ip: K=3, TN=4)..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv3_IP" \
              -description  "K=3 DW Conv TN=4, 7ns" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. Check: ${proj_name}/solution1/syn/report/csynth.rpt"
close_project
