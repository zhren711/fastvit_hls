#================================================================
# run_csim_r13.tcl -- Phase A round 13: correctness check after replacing
# DW's `kh=step/MAX_K, kw=step%MAX_K` (runtime derivation from a shared
# flat counter) with a genuine compile-time-bounded nested (kh,kw) loop.
# PW keeps its own step loop, now textually separate (own LANE_D/R/C
# copy) -- whether this reintroduces DSP duplication is checked via
# csynth, not here; this only checks correctness.
#================================================================

set proj_name  "mac_array_poc_r13_csim"
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

puts ">>> Running C Simulation (round 13 compile-time-kh-kw correctness check)..."
csim_design

puts ">>> Done."
exit
