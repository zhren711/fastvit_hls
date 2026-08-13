#================================================================
# run_csim.tcl -- csim-only run for the isolated dsconv_fused worker.
# 用法: vitis_hls -f run_csim.tcl
#================================================================

set proj_name "dsconv_fused_proj"
set top_func  "dsconv_worker"

open_project -reset $proj_name
set_top $top_func

add_files dsconv_worker.cpp -cflags "-std=c++14"
add_files -tb tb_dsconv_worker.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 5 -name default

puts ">>> Running C Simulation..."
csim_design

puts ">>> Done. Check dsconv_fused_proj/solution1/csim/report"
close_project
exit
