#================================================================
# run_csim_only.tcl -- re-run just csim_design on the existing
# fastvit_ip_w8a4_proj/solution1 to execute the Tier B Phase B3
# golden-vector dump added to fastvit_ip_tb.cpp (writes .hex files to
# tier_b_rtl/golden/), without re-running csynth/export (much faster).
# 用法: vitis_hls -f run_csim_only.tcl
#================================================================
open_project fastvit_ip_w8a4_proj
open_solution "solution1"
puts ">>> Running C Simulation (golden vector dump only)..."
csim_design
puts ">>> Done."
close_project
exit
