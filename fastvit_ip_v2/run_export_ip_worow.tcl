# run_export_ip_worow.tcl -- ZHR-92 (2026-09-15): PW_WRITEOUT_ROW (FAST instance writes a
# 4-byte row per writeout iteration, 16 -> 4 per (ot,tile); DEFER_WRESP pop moved to the
# compute phase) on top of the deployed wburst baseline (+0.180ns, 87.15% LUT). Isolated
# LUT +1,008 (datapath: 4 clip_shift lanes). Judgment: route-only WNS >= 0; board entry3
# first (cin=48, writeout was half its iterations, ~-25%), entry66 (~-4%); PW 160.6 ->
# 132-137 (as modeled) / 140-150 (silicon stall: check the DEFER pop distance, shortest
# on cin=48) / >=155 (writeout iterations were never 1 cycle); network 253 -> ~226-228.
# *_whoist ARM binaries (register map unchanged).
set proj_name  "worow_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_WRITEOUT_ROW"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_WRITEOUT_ROW"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_WRITEOUT_ROW"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

