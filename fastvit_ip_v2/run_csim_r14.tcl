#================================================================
# run_csim_r14.tcl -- Phase A round 14: correctness check after extracting
# drive_mac() as its own non-inlined, function-pipelined unit (zero
# runtime addressing inside it), called from two ordinary (non-PIPELINE)
# loops -- DW's compile-time (kh,kw) tap nest and PW's step loop. Tests
# whether HLS shares one instance across two call sites when neither is
# itself inside a caller-owned PIPELINE region.
#================================================================

set proj_name  "mac_array_poc_r14_csim"
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

puts ">>> Running C Simulation (round 14 drive_mac-extraction correctness check)..."
csim_design

puts ">>> Done."
exit
