# run_export_ip_dwrow.tcl -- ZHR-92 (2026-09-15): DWR_ROW_PF + DWR_DEFER_WRESP
# (DW per-row fixed cost: produce read_request one row ahead + consume
# write_response deferred two rows) on top of the deployed merge4 baseline.
# Both flags together, by decision (separately each is masked by the other).
# Isolated LUT +626 (small). Judgment: route-only WNS >= 0 (phys_opt only within
# -0.2), real LUT, then board: DW 68-85 = both sides hidden; 90-105 = one side
# still binding (stride-2 layers tell which); >=110 = not AXI latency, refit.
set proj_name  "dwrow_export"
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
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

