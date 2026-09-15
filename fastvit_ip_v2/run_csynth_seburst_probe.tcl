# run_csynth_seburst_probe.tcl -- ZHR-92 (2026-09-15): SE_BURST (GAP read-side burst +
# SCALE GELU-style read/compute/write burst on elemwise_in/out_burst). csynth only.
# Judgment: readreq/writereq/writeresp outside the chunk loops, read/write inside
# at II=1 (GELU/ADD-isomorphic); zero II violations; resource delta small (ports
# reused; ch_sum 4KB + gate_buf 1KB); no new 32x32 multiply.
set proj_name  "seburst_probe"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DSE_BURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DSE_BURST"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DSE_BURST"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

