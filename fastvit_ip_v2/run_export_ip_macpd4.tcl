# run_export_ip_macpd4.tcl -- ZHR-92 (2026-09-05): isolated csynth + export
# for MAC_PD=1->4 (2nd step of the widening line, after MAC_PD=2 closed as
# a real win). Confirmed via run_csynth_macpd4_nopart.tcl: PW_FLAT achieved
# II=1 automatically (HLS auto-inferred cyclic factor=2 on pw_weight_cache
# via the pipeline pragma, no manual partition needed) -- isolated LUT
# 70,661 (+25.4% over MAC_PD=2's 56,358), BRAM_18K 243 (+15.2%), DSP
# unchanged. Real P&R run despite the elevated isolated LUT projection
# (~84% real, above every prior successful closure on this line) because
# isolated-vs-real divergence direction is not reliably predictable on
# this project (MAC_PD=1->2's own isolated->real ratio was favorable).
set proj_name  "macpd4"
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
