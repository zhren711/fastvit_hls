#================================================================
# run_csim_a2_fpg.tcl -- A2 pre-step: fpg (filters-per-group) fix. Real
# network has 4 DW layers at fpg=2 (3 stage-downsamples + final_conv),
# silently wrong before this fix -- fpg was carried in LayerDescV2 but
# never read in mac_array.cpp. Judgement this round is csim only
# (resources evaluated together with the parallelism drop next round).
#================================================================

set proj_name  "mac_array_poc_a2_fpg_csim"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation (A2 pre-step: fpg=2)..."
csim_design

puts ">>> Done."
exit
