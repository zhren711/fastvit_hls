#================================================================
# run_hls_v18_200mhz.tcl — fastvit_ip v1.9: same v1.8 grouped-master
# topology (conv/dwconv/pwconv share gmem0-3, add/gelu share gmem4-6),
# re-scheduled at clk_period=5ns (200MHz target).
#
# v1.8 already validated at 100MHz: WNS=+0.250ns, LUT=67.10% (placed
# successfully, unlike v1.5's full-independence which failed to place
# even at 100MHz). This re-export tests whether the physical separation
# of add/gelu's FIFO logic from conv/dwconv/pwconv's actually helps close
# the 2.87ns WNS gap that blocked 200MHz on v1.2 (4 shared masters, all
# 5 ops), or whether the same op_code-fanout problem just re-appears
# inside each smaller group.
#
# Separate project dir (fastvit_ip_proj_v18_200mhz) so this never
# touches fastvit_ip_proj_v18 (the validated 100MHz v1.8 baseline) or
# the canonical v1.2 fastvit_ip_proj (matches the deployed board).
#
# 用法: vitis_hls -f run_hls_v18_200mhz.tcl
#================================================================

set proj_name  "fastvit_ip_proj_v18_200mhz"
set top_func   "fastvit_ip"
set part       "xc7z020clg400-1"
set clk_period "5"

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

puts ">>> Exporting IP v1.9 (v1.8 grouped-master topology, clk_period=5ns/200MHz target)..."
export_design -format ip_catalog \
              -display_name "FastVIT_Unified_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, op_code dispatched, xc7z020, grouped m_axi masters (gmem0-3 heavy ops, gmem4-6 add/gelu), 200MHz target" \
              -vendor        "user.org" \
              -version       "1.9"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
exit
