#================================================================
# run_csynth_u64_r4.tcl -- Phase A round 4: csynth of the restructured
# (cyclic-partition + shared-control OUTER/INNER) mac_array.cpp at
# MAC_UNROLL_FACTOR=64 -- single-variable comparison against round 3's
# mac_array_poc_u64 (same factor, old ARRAY_PARTITION-complete/flat-UNROLL
# structure: 151 DSP / 76163 FF / 466785 LUT / 0 BRAM).
#================================================================

set proj_name  "mac_array_poc_u64_r4"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14 -DMAC_UNROLL_FACTOR=64"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14 -DMAC_UNROLL_FACTOR=64"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/mac_array_top_csynth.rpt"
exit
