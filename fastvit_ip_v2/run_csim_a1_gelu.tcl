#================================================================
# run_csim_a1_gelu.tcl -- Phase A1: GELU (atomic hardware op, single
# source; ONNX-side Div/Erf/Add/Mul folding is generator work, A2). Last
# operator for A1's coverage.
#================================================================

set proj_name  "mac_array_poc_a1_gelu_csim"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation (Phase A1: GELU)..."
csim_design

puts ">>> Done."
exit
