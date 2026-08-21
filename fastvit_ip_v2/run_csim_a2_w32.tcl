#================================================================
# run_csim_a2_w32.tcl -- A2 pre-step round 2: array width 64->32 (4x4x2).
# Geometry-only change, no logic change -- quick correctness re-check
# before the resource measurement.
#================================================================

set proj_name  "mac_array_poc_a2_w32_csim"
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

puts ">>> Running C Simulation (A2 pre-step: 32-wide array)..."
csim_design

puts ">>> Done."
exit
