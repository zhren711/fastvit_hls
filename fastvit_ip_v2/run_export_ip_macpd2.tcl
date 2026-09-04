# run_export_ip_macpd2.tcl -- ZHR-92 (2026-09-04): isolated csynth + export
# for the MAC_PD=1->2 re-check (weight-residency premise: PW_FLAT no longer
# reads gmem_w on the hot/cached path, and the dead uncached fallback arm
# has now been structurally removed from the hot loop by default -- see
# mac_array_raster_integrated.cpp's own header comment at the pw_cached
# branch, and this round's CLAUDE.md/Linear write-up). Confirmed via
# run_csynth_macpd2_v2.tcl: PW_FLAT achieved II=1 (both instances), isolated
# LUT 56,358 / DSP 50 / BRAM_18K 211. Fresh project name per this project's
# export_design stale-cache discipline.
set proj_name  "macpd2"
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
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit
