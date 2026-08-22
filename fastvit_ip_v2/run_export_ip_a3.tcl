#================================================================
# run_export_ip_a3.tcl -- A3 first P&R (ZHR-92, 2026-08-21): package the
# real-AXI mac_array_top solution (32-wide, round-2-reverted state, 113
# DSP / 42,616 LUT csynth) as a Vivado IP for block-design integration.
#================================================================

set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

open_solution "solution1"
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog -vendor user.org -library hls -version 1.0

puts ">>> Done. IP at: ${proj_name}/solution1/impl/ip"
exit
