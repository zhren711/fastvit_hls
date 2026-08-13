# run_w8a4_aggressive_fanout_round2.tcl — second round of
# phys_opt_design on top of the checkpoint already improved by round 1
# (WNS -2.563ns -> -2.397ns). Diminishing returns are likely (most
# fanout-opt techniques converge after one pass) but cheap to check.
#
# 用法: vivado -mode batch -source run_w8a4_aggressive_fanout_round2.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_w8a4_200mhz_proj"
set ckpt_in    "$proj_dir/fastvit_bd_wrapper_w8a4_aggressivefanout.dcp"

puts ">>> Opening round-1 checkpoint: $ckpt_in"
open_checkpoint $ckpt_in

set wns_before [get_property SLACK [get_timing_paths -max_paths 1 -delay_type max]]
puts ">>> WNS before round 2: $wns_before ns"

puts ">>> Running phys_opt_design -directive AggressiveFanoutOpt (round 2)..."
phys_opt_design -directive AggressiveFanoutOpt

set wns_after [get_property SLACK [get_timing_paths -max_paths 1 -delay_type max]]
puts ""
puts "========================================"
puts ">>> WNS after round 2: $wns_after ns  (round 1 result: -2.397ns; original baseline: -2.563ns)"
puts "========================================"

set ckpt_out "$proj_dir/fastvit_bd_wrapper_w8a4_aggressivefanout_r2.dcp"
write_checkpoint -force $ckpt_out
puts ">>> Checkpoint saved: $ckpt_out"
puts ">>> Done."
