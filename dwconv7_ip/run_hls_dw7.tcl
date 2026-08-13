#================================================================
# run_hls_dw7.tcl — dwconv7_ip v1.0 综合脚本
#   K=7 专用 DW Conv, TN=2 并行
#   目标: xc7z020clg400-1, 7ns (142MHz, 留时序余量)
#
# 用法: vitis_hls -f run_hls_dw7.tcl
#================================================================

set proj_name "dwconv7_ip_proj"
set top_func  "dwconv7_ip"
set part      "xc7z020clg400-1"

open_project $proj_name
set_top $top_func

add_files dwconv7_ip.cpp -cflags "-std=c++14"
add_files dwconv7_ip.h   -cflags "-std=c++14"

open_solution "solution1" -reset -flow_target vivado
set_part $part
create_clock -period 7 -name default

puts ">>> C Synthesis (dwconv7_ip: K=7, TN=2)..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv7_IP" \
              -description  "K=7 DW Conv TN=2, 7ns" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. Check: ${proj_name}/solution1/syn/report/csynth.rpt"
close_project
