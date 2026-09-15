# run_export_ip_merge4.tcl -- ZHR-92 (2026-09-15): PW_ROWREAD_MERGE4 (one ROW_READ
# request per (rt,ci) covering the tile's 4 contiguous input rows; runtime FILL
# trip count; explicit-bank (word<<2)|lane store indexing, FILL II=1; per-layer
# W%4==0 guard with the original per-(rr,ci) path kept for W<4) on top of the
# deployed pwpf baseline. Isolated LUT +2,106 (both paths synthesized) -- the
# largest delta on this line; phys_opt allowed if route-only WNS is within -0.2.
# Pre-registered board reading: the W=8 layers (entries 60/64/66/70/72) should
# move MOST (119-4n cycles saved per (rt,ci) group, most groups), the W=64
# layers (3/7/9/13/15) least. PW 231 -> ? (model ~-44ms).
set proj_name  "merge4_export"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_ROWREAD_MERGE4"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_ROWREAD_MERGE4"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_ROWREAD_MERGE4"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

