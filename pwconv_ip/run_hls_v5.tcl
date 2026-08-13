#================================================================
# run_hls_v5.tcl — pwconv_ip v5 综合脚本
#   Input-Stationary + 64-bit AXI + in_tile_cache BRAM
#   目标: xc7z020clg400-1, 200MHz (5ns)
# 用法: vitis_hls -f run_hls_v5.tcl
#================================================================

set proj_name "pwconv_ip_proj"
set top_func  "pwconv_ip"
set part      "xc7z020clg400-1"

open_project $proj_name
set_top $top_func

add_files pwconv_ip.cpp -cflags "-std=c++14"
add_files pwconv_ip.h   -cflags "-std=c++14"

open_solution "solution1" -reset -flow_target vivado
set_part $part
create_clock -period 7 -name default

puts ">>> C Synthesis (v6: 32-bit native AXI, 4x bandwidth)..."
csynth_design

puts ">>> Exporting IP..."
export_design -format ip_catalog \
              -display_name "FastVIT_PWConv_IP_v6" \
              -description  "1x1 Conv 32-bit AXI 4x bandwidth, TM=4 TN=4 TS=16, 7ns" \
              -vendor        "user.org" \
              -version       "17.0"

puts ">>> Done. Check: ${proj_name}/solution1/syn/report/csynth.rpt"
close_project
