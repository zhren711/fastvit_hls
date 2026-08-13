#================================================================
# run_cosim.tcl — isolated RTL cosim to measure real cycles/tile
# for dwconv_ip v12.0, using S1B0-DW7-like params (CH=48,K=7,fpg=1)
# but H=W=16 to keep cosim runtime bounded.
# Does NOT touch the production dwconv_ip_proj / solution1.
# Usage: vitis_hls -f run_cosim.tcl
#================================================================

set proj_name  "cosim_dw7_proj"
set top_func   "dwconv_ip"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files            ../dwconv_ip.cpp -cflags "-std=c++14"
add_files            ../dwconv_ip.h   -cflags "-std=c++14"
add_files -tb        tb_cosim_dw7.cpp -cflags "-std=c++14 -I.."

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> C simulation..."
csim_design

puts ">>> C synthesis..."
csynth_design

puts ">>> RTL cosimulation (this measures REAL cycle counts, not static estimates)..."
cosim_design -rtl verilog -trace_level none

puts ">>> Done. Check solution1/sim/report/ for cosim wall-clock/cycle results."
close_project
