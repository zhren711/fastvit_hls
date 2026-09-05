# run_csynth_macpd2.tcl -- ZHR-92 (2026-09-04): isolated csynth-only check
# for MAC_PD=1->2 (weight-residency premise re-check: does PW_FLAT's
# achieved II still hold at 1 now that pw_weight_cache (BRAM, dual-port)
# replaced the old gmem_w (single AXI port) as PW_FLAT's weight source?).
# csynth only, no export -- this step is a pass/fail gate on achieved II
# before spending any more time.
set proj_name  "macpd4nopart"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> csynth done for MAC_PD=2"
exit
