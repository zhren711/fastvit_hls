#================================================================
# run_hls_200mhz.tcl — fastvit_ip_w8a4, same v2.0 topology (4 shared
# m_axi masters, W8A4 quantization), re-scheduled at clk_period=5ns
# (200MHz target) as IP v2.1.
#
# v2.0 already validated at 100MHz: WNS=+0.410ns, LUT=59.01%, DSP=68
# (real Vivado numbers) -- better margin than the v1.2/W8A8 baseline.
# This tests whether the narrower (nibble, not byte) datapath changes
# anything about the op_code-fanout bottleneck that blocked every
# 200MHz attempt on v1.2 (8/8 independent attempts failed there). The
# bottleneck's exact location/signal name is expected to differ from
# the v1.2 diagnosis (different netlist), so report_timing needs to be
# re-run fresh, not assumed to match old findings.
#
# Separate project dir (fastvit_ip_w8a4_proj_200mhz) so this never
# touches the validated 100MHz fastvit_ip_w8a4_proj.
#
# 用法: vitis_hls -f run_hls_200mhz.tcl
#================================================================

set proj_name  "fastvit_ip_w8a4_proj_200mhz"
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

puts ">>> Exporting IP v2.1 (W8A4, clk_period=5ns/200MHz target)..."
export_design -format ip_catalog \
              -display_name "FastVIT_W8A4_IP" \
              -description  "Unified conv/dwconv/pwconv/add/gelu accelerator, W8A4 quantization, op_code dispatched, xc7z020, 200MHz target" \
              -vendor        "user.org" \
              -version       "2.1"

puts ">>> Done. Check solution1/csim/report and solution1/syn/report/fastvit_ip_csynth.rpt"
close_project
exit
