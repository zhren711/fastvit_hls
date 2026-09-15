# run_csynth_dwrow_probe.tcl -- ZHR-92 (2026-09-15): STEP-1 schedule probe for
# DWR_ROW_PF + DWR_DEFER_WRESP (DW per-row fixed cost, ~73 cycles/row x 97,344
# rows = ~71ms of DW 115ms by the merge4 refit). csynth only. Judgment: produce
# readreq stays in the ROW body (now one row ahead) and read() stays inside
# Pipeline_COL at II=1; consume writereq/writeresp stay in the per-row loops,
# write() stays inside CCOL at II=1; no data op leaves its pipeline; no new
# 32x32 multiply anywhere (binding DB); II violations unchanged (zero).
set proj_name  "dwrow_probe"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DDWR_ROW_PF -DDWR_DEFER_WRESP"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

