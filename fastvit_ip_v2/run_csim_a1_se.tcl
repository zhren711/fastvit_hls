#================================================================
# run_csim_a1_se.tcl -- Phase A1: SE block data flow (GAP + fc1 + ReLU +
# fc2 + Sigmoid + broadcast Scale), priority-1 operator per user direction
# (channel-reduction + broadcast most likely to expose buffer/descriptor
# design gaps before A2).
#================================================================

set proj_name  "mac_array_poc_a1_se_csim"
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

puts ">>> Running C Simulation (Phase A1: SE block)..."
csim_design

puts ">>> Done."
exit
