# run_export_ip_sma.tcl -- ZHR-92 (2026-09-12): SHARED_MUL_ARMS -- all seven
# remaining call sites on the shared 32x32 multiplier (run_layer's 6-arm
# grp_fu_1281 + the local U1439's arm, plus the top-level scalar_hw arm)
# converted to accumulators / one per-chunk add-loop / narrow-typed
# multiplies, on top of the deployed sohoist baseline. csim 5/5+4/4+8/8+4/4.
# Judgment, pre-registered: (1) run_layer's bind report has ZERO Multiplier
# ops and the top's remaining multiplies are narrow cores (not
# mul_32s_32s_32); (2) mul_32s_32s_32_2_1 absent from the top-10 critical
# paths; (3) csim (done); (4) route_design-alone WNS vs +0.138 -- expected
# ~+0.25 (the next-worst distinct path, DW -> gmem_act store FIFO, at this
# placement); ~+0.18 would mean the store-unit path bites earlier.
set proj_name  "sma_export"
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

