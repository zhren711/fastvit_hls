#================================================================
# run_hls.tcl — fastvit_ip_w8a4: W8A4 redesign (weights stay ap_int<8>,
# activations shrink to ap_int<4>), starting point copied verbatim from
# the v1.2 baseline (fastvit_ip/run_hls.tcl) before any W8A4 edits are
# applied to the .cpp/.h sources in this directory.
#
# 目标: MicroZed xc7z020clg400-1. See
# C:\Users\zhren\.claude\plans\wondrous-roaming-yeti.md for the full plan
# (Phase 1: quantization/calibration, Phase 2: this HLS redesign, Phase 3:
# 200MHz closure, Phase 4: validation).
#
# 用法: vitis_hls -f run_hls.tcl
#================================================================

set proj_name  "fastvit_ip_w8a4_proj"
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

puts ">>> Exporting IP v2.0 (W8A4: 8-bit weights, 4-bit activations)..."
export_design -format ip_catalog \
              -display_name "FastVIT_W8A4_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, W8A4 quantization, op_code dispatched, xc7z020" \
              -vendor        "user.org" \
              -version       "2.0"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
exit
