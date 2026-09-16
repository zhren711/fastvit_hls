# run_export_ip_flat.tcl -- ZHR-92 (2026-09-16): DWR_FLAT (dwr_produce ROWxCOL and
# dwr_consume CROWxCCOL each one II=1 pipelined loop per channel; row-boundary bus ops on
# provably write-free iterations) on top of the deployed worow baseline (+0.285ns, 86.74%
# LUT). Isolated LUT +2,811 -- the real number is the first gate. Judgment: route-only
# WNS >= 0; board DW 67.8 -> 45-52 (as modeled) / 55-60 (silicon stall) / >=62 (the
# per-row term was not loop glue); network 229 -> ~208-211. *_whoist ARM binaries.
set proj_name  "flat_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DDWR_FLAT"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DDWR_FLAT"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DDWR_FLAT"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

