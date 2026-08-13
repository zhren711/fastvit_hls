#================================================================
# run_hls_v14.tcl — dwconv_ip v14.0 (TR-sequential, fits xc7z020)
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls_v14.tcl
#
# 主要变化 vs v13:
# - COMPUTE_DW: TR 改为 sequential (移至最外层, 不再 UNROLL)
#   + 每 (kh,r) pair 将 dw_in_buf 行预加载到 row_buf (1 cycle)
#   → KW pipeline 只需 13:1 MUX 而非 169:1, 控制集从 ~2000 降至 ~200
#   → 适配 xc7z020 LUT/FF 容量 (v13 因 1995 控制集导致 placement 失败)
# - 移除 FILM-QNN DSP 打包 (dsp_packed_mac2), 使用直接 MAC
# - 每 tile 周期数: DW_TR × Kh × (Kw + depth) ≈ 4×7×12 = 336 (vs v13 的 56)
# - 预期 DW7 (C=48, 64×64): ~50ms (vs v10 的 1030ms, ~20× 加速)
#================================================================

set proj_name  "dwconv_ip_proj_v14"
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

puts ">>> Running C Synthesis (v14.0 TR-sequential, resource-optimized)..."
csynth_design

puts ">>> Exporting IP v14.0..."
export_design -format ip_catalog \
              -display_name "FastVIT_DWConv_IP" \
              -description  "DW Conv v14 TR-seq KW-pipeline TC-unroll, fits xc7z020" \
              -vendor        "user.org" \
              -version       "14.0"

puts ">>> Done. Check solution1/syn/report/dwconv_ip_csynth.rpt for resource usage."
close_project
