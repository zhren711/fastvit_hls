#================================================================
# run_hls_v12.tcl — dwconv_ip v12.0 (Compute Loop Optimization)
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls_v12.tcl
#
# 主要变化 vs v11:
# - COMPUTE_DW: PIPELINE 从 TC 移至 KW, UNROLL TR/TC
#   → 196 次 pipeline 重启/tile → 7 次, ~40× compute 加速
# - LOAD_DW_IN: PIPELINE 在 r 外层, UNROLL 内层 c
#   ch_in_buf cyclic=16 → 13 列同时读取 (不同 BRAM bank)
# - STORE_DW_OUT: PIPELINE 在 r, UNROLL c
# 预期: DW7 (C=48, 64×64) ~1000ms → ~15ms
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

puts ">>> Running C Synthesis (Compute Loop Optimization v12.0)..."
csynth_design

puts ">>> Exporting IP v12.0..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP" \
              -description  "DW Conv v12 KW-pipeline TR/TC-unroll, xc7z020" \
              -vendor        "user.org" \
              -version       "12.0"

puts ">>> Done. Check solution1/syn/report/dwconv_ip_csynth.rpt for resource usage."
close_project
