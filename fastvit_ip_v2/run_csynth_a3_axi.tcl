#================================================================
# run_csynth_a3_axi.tcl -- A3 first step: real AXI interface resource
# budget check (ZHR-92, 2026-08-21). First csynth run with actual
# #pragma HLS INTERFACE m_axi/s_axilite on mac_array_top (4 masters:
# gmem_act shared in/out, gmem_w, gmem_b, gmem_meta for desc/out_written)
# -- every prior resource number (round 5-15, A1, A2 pre-step) was
# compute-core-only. Answers: does AXI infrastructure fit the current
# 32-wide (4x4x2) budget, before any further A3 work.
#================================================================

set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/mac_array_top_csynth.rpt"
exit
