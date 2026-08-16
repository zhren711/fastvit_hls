#================================================================
# run_sweep_w8a4.tcl -- Phase A round 3, W8A4 re-check of the 512 point.
# Triggered condition (2026-08-16 direction): only rerun at W8A4 if the
# 512 point exceeds budget at W8A8 -- it does (1047 DSP, 795374 LUT vs
# 220/53200 available), so this runs.
#================================================================

set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

set proj_name "mac_array_poc_u512_w8a4"
puts ">>> ================= MAC_UNROLL_FACTOR=512, ACT_BITS=4 (W8A4) ================="
open_project -reset $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14 -DMAC_UNROLL_FACTOR=512 -DACT_BITS=4"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14 -DMAC_UNROLL_FACTOR=512 -DACT_BITS=4"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csynth_design
puts ">>> Done. Report: ${proj_name}/solution1/syn/report/mac_array_top_csynth.rpt"
exit
