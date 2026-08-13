#================================================================
# run_hls_v9.tcl — dwconv_ip v9.0 (+ fpg expand factor)
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls_v9.tcl
#================================================================

set proj_name  "dwconv_ip_proj"
set top_func   "dwconv_ip"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Synthesis (no sim, just check resources)..."
csynth_design

puts ">>> Exporting IP v9.0..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP" \
              -description  "DW Conv TN=1, fpg support, K=3/7, xc7z020" \
              -vendor        "user.org" \
              -version       "9.0"

puts ">>> Done. Check solution1/syn/report/dwconv_ip_csynth.rpt for resource usage."
close_project
