# run_export_ip_dw_out_reuse.tcl -- ZHR-92 (2026-09-12): export the
# DW_OUTPUT_BURST *port-reuse* build for real P&R. Same dwr_writeout_packed
# mechanism as the 2026-09-08/09 6th-port build (wbuf[g]'s 4 bytes packed
# into one ap_uint<32> write, CROW_CCOL achieved II 8 -> 2), but the packed
# write now goes through PW_FLAT's EXISTING out_burst port (passed into
# run_dw_layer_raster) instead of a new dw_out_burst port on gmem_act.
#
# Why: the 6th-port build's real P&R came back WNS=-0.418ns (route_design
# alone, vs the baseline's own -0.167ns) with the critical path routed
# through gmem_act's store-unit write-request FIFO at mem_reg[5][65] (one
# bit wider than the [64] of a build with one fewer write port) -- the
# extra port widened the shared adapter's request FIFO/arbitration and
# landed it on the already-marginal shared-multiplier path. DW and PW are
# strictly mutually exclusive (run_layer early-returns for DW), so one
# port can serve both.
#
# csim clean on the reuse source (2026-09-11): dw_raster_layer_tb 5/5,
# wiring tb 4/4, gelu_add_burst_tb 8/8. Isolated csynth (dw_input_burst/s1,
# 2026-09-11): CROW_CCOL achieved II=2 preserved, single 200-880 carried-
# dependence violation (structural minimum, as before), LUT 72,639 (-128 vs
# the 6th-port build's 72,767, -1,290 vs the elemwise_burst baseline's
# 73,929), BRAM_18K 245 / DSP 56 flat. Register map identical to HEAD's
# driver (DW_IN_BURST at 0x100 is the last register; no DW_OUT_BURST).
#
# Acceptance criterion for the P&R that follows (pre-registered): the
# store-unit FIFO width in the timing report returns to mem_reg[5][64],
# and WNS returns to at least the baseline's own route_design-alone value
# (-0.167ns), inside the ~-0.2ns phys_opt_design recovery band.
#
# Fresh project name, fresh export -- per this project's own
# "export_design silently reuses cached HDL" rule.
set proj_name  "dw_out_reuse_export"
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
