#================================================================
# run_hls_200mhz.tcl -- csim + csynth + IP export for the isolated
# dsconv_fused (dsconv_worker) kernel, 200MHz target (clk_period=5ns),
# matching every prior isolated-worker run in this investigation for a
# fair comparison.
# 用法: vitis_hls -f run_hls_200mhz.tcl
#================================================================

set proj_name "dsconv_fused_proj"
set top_func  "dsconv_worker"
set part      "xc7z020clg400-1"
set clk_period "5"

open_project $proj_name
set_top $top_func

add_files dsconv_worker.cpp -cflags "-std=c++14"
add_files -tb tb_dsconv_worker.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation..."
csim_design

puts ">>> Running C Synthesis..."
csynth_design

puts ">>> Exporting IP (dsconv_fused, clk_period=5ns/200MHz target)..."
export_design -format ip_catalog \
              -display_name "DSCONV_FUSED" \
              -description  "Fused DW7+PW1 on-chip patch-level dataflow, W8A4, isolated feasibility study, xc7z020, 200MHz target" \
              -vendor        "user.org" \
              -version       "1.0"

puts ">>> Done. Check dsconv_fused_proj/solution1/syn/report/dsconv_worker_csynth.rpt"
close_project
exit
