# run_export_ip_rowburst.tcl -- ZHR-92 (2026-09-14): DWR_ROWBURST (step 2:
# lane-0 row burst request before CCOL, lane-1 per-row buffer drained after,
# responses after; write(word) stays in the pipeline) on top of the deployed
# dwob baseline (SHARED_MUL_ARMS + DW_OUTPUT_BURST). csim 5/5+4/4+8/8+4/4;
# csynth: CCOL achieved II=1 (was 2 -- the 200-880 two-writes-per-iteration
# dependence is gone), writereq/writeresp outside the pipeline, isolated LUT
# +788, DSP -10. Judgment: route_design-alone WNS >= 0 (current +0.172),
# real LUT vs the isolated +788, then board.
set proj_name  "rowburst_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_ROWBURST"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DDWR_ROWBURST"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

