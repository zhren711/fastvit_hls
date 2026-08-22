#================================================================
# run_ckpt_dump.tcl -- A2 exit measurement: run the real 82-entry
# hardware sequence against Stem's ARM-computed output + real gamma-
# folded weights, dumping 6 checkpoints for the segmented cosine table.
#================================================================

set proj_name  "mac_array_ckpt_dump"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_ckpt_dump.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running A2 exit checkpoint dump..."
csim_design

puts ">>> Done."
exit
