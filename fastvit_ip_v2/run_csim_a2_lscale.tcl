#================================================================
# run_csim_a2_lscale.tcl -- A2: layer_scale (LDESC_OP_LSCALE), the
# operator gap found in the A2 design pass. Phase10 exercises the real
# block pattern: DW(identity) + PW(fc2 stand-in) -> LSCALE -> Add.
#================================================================

set proj_name  "mac_array_poc_a2_lscale_csim"
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

puts ">>> Running C Simulation (A2: layer_scale)..."
csim_design

puts ">>> Done."
exit
