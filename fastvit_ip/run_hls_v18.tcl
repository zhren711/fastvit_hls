#================================================================
# run_hls_v18.tcl — fastvit_ip v1.8: grouped-master experiment
# (conv/dwconv/pwconv keep sharing gmem0-3; add/gelu move to their own
# gmem4-6 group). clk_period=7ns/100MHz -- validate the topology places
# on KNOWN-GOOD timing first, per this project's established practice
# (v1.5's fully-independent-master variant was also checked at 100MHz
# first, and that's what caught its placement failure early).
#
# Separate project dir (fastvit_ip_proj_v18) so this never touches the
# canonical v1.2 fastvit_ip_proj that matches the deployed board.
#
# 用法: vitis_hls -f run_hls_v18.tcl
#================================================================

set proj_name  "fastvit_ip_proj_v18"
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

puts ">>> Exporting IP v1.8 (grouped m_axi masters: gmem0-3 shared by conv/dwconv/pwconv, gmem4-6 shared by add/gelu -- 7 masters total, not 4 or 15)..."
export_design -format ip_catalog \
              -display_name "FastVIT_Unified_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, op_code dispatched, xc7z020, grouped m_axi masters (gmem0-3 heavy ops, gmem4-6 add/gelu)" \
              -vendor        "user.org" \
              -version       "1.8"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
exit
