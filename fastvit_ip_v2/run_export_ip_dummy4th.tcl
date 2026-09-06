# run_export_ip_dummy4th.tcl -- ZHR-92 (2026-09-05): isolated csynth +
# export for the DUMMY_FOURTH_MASTER diagnostic build (restores sc_data's
# NUM_SI to 4 via a genuinely-live-but-never-triggered 4th m_axi master,
# isolating "does 4-way arbitration matter" from "does gmem_meta's own
# traffic matter" for the GELU/ADD regression bisected to the gmem_meta
# elimination round). csim clean 4/4 (mac_array_raster_integrated_wiring_
# tb.cpp) before this export. NOT a deployable candidate -- diagnostic only.
set proj_name  "dummy4th"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb mac_array_raster_integrated_wiring_tb.cpp -cflags "-std=c++14"

open_solution "s1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog
puts ">>> Export done: ${proj_name}/s1/impl/ip"
exit
