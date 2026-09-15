# run_csynth_whoist_probe.tcl -- ZHR-92 (2026-09-15): PW_WHOIST_WIDE step-1 probe (32-bit
# burst_maxi w_burst on gmem_w feeding PW_WEIGHT_HOIST one word/cycle). Judgment:
# readreq outside PW_WEIGHT_HOIST_W, read inside at II=1 (no pw_weight_cache write
# port violation -- 4 byte stores per iteration into the cache), zero II violations;
# resource delta small; no new 32x32 multiply.
set proj_name  "dwwb_probe"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE -DDWR_WBURST"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE -DDWR_WBURST"
add_files -tb gelu_add_burst_tb.cpp -cflags "-std=c++14 -DPW_WHOIST_WIDE -DDWR_WBURST"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
# (csynth only -- default-build equivalence check)
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit

