#================================================================
# run_csynth_diag_fixedidx.tcl -- ONE-SHOT DIAGNOSTIC, not a round.
# Temporarily replaces DW's runtime-indexed gather
# (dw_patch[dd][rr*dw_S+kh][cw*dw_S+kw]) with a fixed compile-time index
# (dw_patch[dd][rr][cw]) to test the hypothesis that run_reduce_unified's
# 99k LUT jump over round 8's PW-alone baseline (35k) comes from the
# 36:1 per-lane mux this dynamic index requires. Answer will be WRONG
# (diagnostic only) -- source is reverted immediately after this run,
# never committed.
#================================================================

set proj_name  "mac_array_poc_diag_fixedidx"
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
