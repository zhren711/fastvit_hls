# run_csynth_preg_probe.tcl -- ZHR-92 (2026-09-16): layer (2) step-1 probe on top of CTR_NARROW:
# PW_MAC_PREG (product registered, BIND_OP latency=1) + PW_ACC_NARROW (ap_int<26> lane acc).
# Judgment: PW_FLAT II=1, pipeline depth +1, Estimated of every module unchanged or better,
# the mul core in the bind DB is a registered variant. csynth at 10ns.
set proj_name  "preg_probe"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

