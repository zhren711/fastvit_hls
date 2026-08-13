#================================================================
# run_csim.tcl -- csim-only run for the isolated dwconv_linebuf worker.
# 用法: vitis_hls -f run_csim.tcl
#================================================================

set proj_name "dwconv_linebuf_proj"
set top_func  "dwconv_worker"

open_project -reset $proj_name
set_top $top_func

add_files dwconv_worker.cpp -cflags "-std=c++14"
add_files -tb tb_dwconv_worker.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 5 -name default

puts ">>> Running C Simulation..."
csim_design

puts ">>> Done. Check dwconv_linebuf_proj/solution1/csim/report"
close_project
exit
