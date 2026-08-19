#================================================================
# run_csynth_r5.tcl -- Phase A round 5: csynth of the fully restructured
# mac_array.cpp (true 512-physical-MAC geometry: DW pd=channel/pr*pc=
# spatial/K*K time-multiplexed; PW pd=Cin-reduction-chunk/pr*pc=spatial,
# Cout processed sequentially). No more factor sweep -- this IS the
# paper's 8x8x8=512 design now, tested once.
#================================================================

set proj_name  "mac_array_poc_r5"
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

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/mac_array_top_csynth.rpt"
exit
