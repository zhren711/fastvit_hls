#================================================================
# run_csim.tcl -- Phase A mac_array PoC, csim-only (no csynth/export).
# 目标: MicroZed xc7z020clg400-1 (kept consistent with fastvit_ip/ even
# though this round is structural-correctness-only, not timing).
#================================================================

set proj_name  "mac_array_poc"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation only..."
csim_design

puts ">>> Done. Check mac_array_poc/solution1/csim/report"
exit
