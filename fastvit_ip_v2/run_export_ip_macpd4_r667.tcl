# run_export_ip_macpd4_r667.tcl -- ZHR-92 150MHz re-check, point 1/3 (9.0ns/111MHz).
# Re-does the 111/125/150MHz sweep on the CURRENT architecture (MAC_PD=4,
# gmem_meta eliminated, WNS+0.339ns at 10ns/100MHz) -- the original sweep's
# closure premise (critical path = gmem_meta's AXI load FIFO, never
# HLS-rescheduled, only Vivado-constraint-tightened) no longer holds:
# gmem_meta doesn't exist anymore, and a later round proved HLS-side
# rescheduling is a real lever (8ns reschedule: WNS -2.129->-1.204ns) but
# non-monotonic (6.67ns reschedule regressed to -3.490ns) on the OLD
# (MAC_PD=1, pre-gmem_meta-elimination) architecture. This sweep re-does the
# reschedule on both sides (HLS create_clock AND Vivado XDC/PS7 freq
# together, which the original 111/125/150 sweep never did -- it only
# tightened the Vivado constraint).
set proj_name  "macpd4_r667"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "6.67"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit
