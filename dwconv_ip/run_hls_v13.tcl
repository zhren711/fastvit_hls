#================================================================
# run_hls_v13.tcl — dwconv_ip v13.0 (FILM-QNN-style DSP packing)
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls_v13.tcl
#
# 主要变化 vs v12:
# - COMPUTE_DW_KH_KW: 借鉴 FILM-QNN (Sun et al., FPGA'22) 的 DSP48E1 packing
#   技术 (Fig.1(b))，扩展到 8x8-bit (原论文是 8x5-bit)。每个 (kh,kw) 周期，
#   共享同一 weight 的两个 activation 通过 DSP48E1 的 (A+D)*B 预加法器打包进
#   一次乘法，DW_TR*DW_TC/2=8 个物理 DSP 完成原本 16 个乘法的工作。
# - 使用独立项目目录，不覆盖 v12 (dwconv_ip_proj/solution1) 的已部署基线报告。
# 预期: COMPUTE_DW 相关 DSP 用量减半，II=1 和资源之外的时序/延迟应基本不变
#================================================================

set proj_name  "dwconv_ip_proj_v13"
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

puts ">>> Running C Synthesis (v13.0 FILM-QNN DSP packing)..."
csynth_design

puts ">>> Exporting IP v13.0..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP" \
              -description  "DW Conv v13 FILM-QNN DSP48E1 packing (2 MAC/DSP), xc7z020" \
              -vendor        "user.org" \
              -version       "13.0"

puts ">>> Done. Check solution1/syn/report/dwconv_ip_csynth.rpt for resource usage."
close_project
