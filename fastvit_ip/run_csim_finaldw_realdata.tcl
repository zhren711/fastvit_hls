#================================================================
# run_csim_finaldw_realdata.tcl -- Phase 0.7 step 4 (2026-08-14)
# Isolated csim of dwconv_worker() alone (the exact function
# fastvit_ip's OP_DWCONV dispatches straight to) against the real
# captured FinalDW weight/bias/activation bytes from the board, to
# check whether the C++ source itself reproduces the real hardware's
# degenerate {-1,0} output or the healthy documented-math reference.
#================================================================

set proj_name  "fastvit_ip_csim_finaldw_realdata"
set top_func   "dwconv_worker"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files dwconv_worker.cpp -cflags "-std=c++14"
add_files -tb dwconv_finaldw_realdata_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation on real FinalDW data..."
csim_design -argv "E:/codes/microzed/fastvit_hls"

puts ">>> Done. Check fastvit_ip_csim_finaldw_realdata/solution1/csim/report"
close_project
