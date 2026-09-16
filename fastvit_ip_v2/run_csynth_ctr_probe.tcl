# run_csynth_ctr_probe.tcl -- ZHR-92 (2026-09-16): CTR_NARROW step-1 probe (the four
# counter chains named by HLS 200-1016 in the frequency sweep: PW_FLAT w_pending 9.30,
# DWR_CONSUME_FLAT l1_nbuf 8.20, PW_FLAT cbase_idx/k 7.80, ROW_READ_FILL4 col_w 7.39).
# Judgment: each module Estimated <= 8.5ns; II=1; zero violations. csynth at 10ns.
set proj_name  "ctr_probe"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DCTR_NARROW"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DCTR_NARROW"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DCTR_NARROW"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

