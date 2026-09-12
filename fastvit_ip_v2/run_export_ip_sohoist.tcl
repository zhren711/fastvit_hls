# run_export_ip_sohoist.tcl -- ZHR-92 (2026-09-12): SCALAR_OP_SIZE_HOIST --
# hw = h_in*w_in and total = cin*hw computed ONCE, unconditionally, in
# mac_array_top before the op_type switch and passed as scalar parameters
# into the six scalar ops (RELU/SIGMOID/GELU/ADD/GAP/SCALE), replacing the
# 10 op_type-gated multiply call sites HLS had been muxing into the
# top-level shared 32x32 multiplier (the sink of every thin-margin critical
# path on this line). DW_OUTPUT_BURST gated OFF -> single-variable change
# on the deployed baseline's own source.
#
# csim 5/5 + 4/4 + 8/8 clean (run_csim_sohoist_all.tcl). Judgment, in
# order: (1) binding database: the shared multiplier's opset no longer
# carries the scalar ops' multiplies; (2) csim (done); (3) real P&R
# route_design-alone WNS vs the baseline's own -0.167ns -- the SIZE of the
# improvement decides whether DW_OUTPUT_BURST can be re-opened (>=0.2ns) or
# the sink's bottleneck is elsewhere (~0.05ns); (4) resources (expected
# slightly down). Fresh project name, fresh export.
set proj_name  "sohoist_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

