# run_export_ip_elemwise_burst.tcl -- ZHR-92 (2026-09-06): export the
# ELEMWISE_BURST build (word-packed hls::burst_maxi<ap_uint<32>> for
# GELU/ADD) for real P&R. csim clean 8/8 (gelu_add_burst_tb.cpp) + DW
# wiring 4/4. Isolated csynth already confirmed: burst inference on all 5
# access points (ManualBurstInstancePassed, Length=variable), ADD's own
# achieved II back to 1 (bonus -- sequential reads resolved the old
# gmem_act port contention), GELU II=1 unchanged, LUT +4.6%/DSP+12%/
# BRAM+0.8% isolated deltas over the macpd4 baseline.
set proj_name  "elemwise_burst"
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
