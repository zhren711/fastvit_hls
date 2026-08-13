#================================================================
# run_hls_200mhz.tcl — fastvit_ip v1.2 topology (4 shared m_axi masters),
# re-exported at clk_period=5ns (200MHz target) as IP v1.6, into a
# SEPARATE project dir (fastvit_ip_proj_200mhz) so it never clobbers the
# canonical v1.2 (100MHz-targeted) fastvit_ip_proj that matches the
# deployed board.
#
# Purpose: reproduce the known WNS=-2.873ns 200MHz baseline (root cause:
# op_code register fanout into shared m_axi FIFO adapter logic, see
# fastvit_ip.h header) so a targeted MAX_FANOUT constraint on
# op_code_read_reg can be tried at the opt_design stage (pre-placement,
# giving the placer real freedom to spread replicated registers) instead
# of the post-route phys_opt_design -directive AggressiveFanoutOpt that
# crashed twice before (see run_fanout_experiment*.log).
#
# 用法: vitis_hls -f run_hls_200mhz.tcl
#================================================================

set proj_name  "fastvit_ip_proj_200mhz"
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

puts ">>> Exporting IP v1.6 (v1.2 topology, 4 shared m_axi masters, clk_period=5ns/200MHz target -- for the op_code MAX_FANOUT experiment)..."
export_design -format ip_catalog \
              -display_name "FastVIT_Unified_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, op_code dispatched, xc7z020, 4 shared m_axi masters, 200MHz target" \
              -vendor        "user.org" \
              -version       "1.6"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
