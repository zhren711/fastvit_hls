#================================================================
# run_hls_200mhz_pwtn4.tcl — fastvit_ip v1.2 topology (4 shared m_axi
# masters), PW_TN reverted 8->4 (see fastvit_ip.h header, 2026-07-31),
# re-exported at clk_period=5ns (200MHz target) as IP v1.7, into a
# SEPARATE project dir (fastvit_ip_proj_200mhz_pwtn4) so it never
# clobbers the canonical v1.2 (100MHz, PW_TN=8) fastvit_ip_proj that
# matches the deployed board.
#
# Purpose: test whether freeing pwconv_worker's LUT/DSP footprint
# (PW_TN=8->4: known 100MHz cost DSP 48.18%->40.91%, LUT 55.82%->50.34%)
# changes the 200MHz WNS. Known 200MHz baseline (v1.4/v1.6, PW_TN=8):
# WNS=-2.873ns, critical path = op_code_read_reg -> shared m_axi FIFO
# adapter routing (75-84% route delay, see fastvit_ip.h / memory).
#
# 用法: vitis_hls -f run_hls_200mhz_pwtn4.tcl
#================================================================

set proj_name  "fastvit_ip_proj_200mhz_pwtn4"
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

puts ">>> Exporting IP v1.7 (v1.2 topology, 4 shared m_axi masters, PW_TN=4, clk_period=5ns/200MHz target -- resource-pressure experiment)..."
export_design -format ip_catalog \
              -display_name "FastVIT_Unified_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, op_code dispatched, xc7z020, 4 shared m_axi masters, PW_TN=4, 200MHz target" \
              -vendor        "user.org" \
              -version       "1.7"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
