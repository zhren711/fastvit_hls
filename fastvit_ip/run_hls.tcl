#================================================================
# run_hls.tcl — fastvit_ip unified IP (merges conv/dwconv/pwconv/add)
# 目标: MicroZed xc7z020clg400-1
# 用法: vitis_hls -f run_hls.tcl
#================================================================

set proj_name  "fastvit_ip_proj"
set top_func   "fastvit_ip"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files fastvit_ip.cpp     -cflags "-std=c++14"
add_files conv_worker.cpp    -cflags "-std=c++14"
add_files dwconv_worker.cpp  -cflags "-std=c++14"
add_files pwconv_worker.cpp  -cflags "-std=c++14"
add_files add_worker.cpp     -cflags "-std=c++14"
add_files gelu_worker.cpp    -cflags "-std=c++14"
add_files -tb fastvit_ip_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation..."
csim_design

puts ">>> Running C Synthesis..."
csynth_design

puts ">>> Exporting IP v1.2 (4 shared m_axi masters, op_code dispatched -- v1.5 dedicated-bundle experiment was tried and reverted, see fastvit_ip.h header)..."
export_design -format ip_catalog \
              -display_name "FastVIT_Unified_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, op_code dispatched, xc7z020, 4 shared m_axi masters" \
              -vendor        "user.org" \
              -version       "1.2"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
