#================================================================
# run_hls_200mhz.tcl -- csim + csynth + IP export for the isolated
# dwconv_linebuf worker, 200MHz target (clk_period=5ns), matching every
# prior isolated-worker run in this investigation
# (fastvit_ip_w8a4/run_hls_200mhz.tcl) for a fair comparison.
# 用法: vitis_hls -f run_hls_200mhz.tcl
#================================================================

set proj_name "dwconv_linebuf_proj"
set top_func  "dwconv_worker"
set part      "xc7z020clg400-1"
set clk_period "5"

open_project $proj_name
set_top $top_func

add_files dwconv_worker.cpp -cflags "-std=c++14"
add_files -tb tb_dwconv_worker.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation..."
csim_design

puts ">>> Running C Synthesis..."
csynth_design

puts ">>> Exporting IP (dwconv_linebuf, clk_period=5ns/200MHz target)..."
export_design -format ip_catalog \
              -display_name "DWCONV_LINEBUF" \
              -description  "Line-buffer/shift-register streaming depthwise conv, W8A4, isolated feasibility study, xc7z020, 200MHz target" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. Check dwconv_linebuf_proj/solution1/syn/report/dwconv_worker_csynth.rpt"
close_project
exit
