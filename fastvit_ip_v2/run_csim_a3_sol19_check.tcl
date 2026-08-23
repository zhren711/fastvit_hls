# run_csim_a3_sol19_check.tcl -- csim for the PW_PATCH_HOIST wide/narrow
# merge round (2026-08-23, ZHR-92): verifies the unified word-read path
# (byte-lane = elem_idx & 3, no more use_wide_path branch) against every
# existing phase, especially Phase6 (layer-50/51-shaped PW, in_off=374 not
# 4-aligned) and Phase15 (the original wide-path shape).
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution19" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csim_design
puts ">>> csim done."
exit
