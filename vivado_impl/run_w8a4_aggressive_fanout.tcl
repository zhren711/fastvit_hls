# run_w8a4_aggressive_fanout.tcl — last remaining Tier A variant, per
# user decision (2026-08-01): phys_opt_design -directive AggressiveFanoutOpt
# as an INCREMENTAL post-route patch (not a pre-constraint -- Tier A's
# pre_opt_design approach was just proven structurally impossible: the
# target cell doesn't exist until AFTER opt_design creates it).
#
# In the W8A8 era (2026-07-31) this exact directive, applied the same way
# (opened on an already-routed checkpoint), showed a REAL partial
# improvement (WNS -2.873ns -> -2.303ns) twice before crashing with no
# diagnostic output (~90% through, exit code 127) both times -- never
# completed. Retesting on W8A4 to see if the narrower datapath changes
# that outcome. Baseline for this build: WNS=-2.562842ns.
#
# Starts from the already-routed checkpoint of the completed (but
# constraint-ineffective) fanoutfix run, so this doesn't need to redo
# synth+place+route from scratch.
#
# 用法: vivado -mode batch -source run_w8a4_aggressive_fanout.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_w8a4_200mhz_proj"
set routed_dcp "$proj_dir/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper_routed.dcp"

puts ">>> Opening routed checkpoint: $routed_dcp"
open_checkpoint $routed_dcp

set wns_before [get_property SLACK [get_timing_paths -max_paths 1 -delay_type max]]
puts ">>> WNS before AggressiveFanoutOpt: $wns_before ns"

puts ">>> Running phys_opt_design -directive AggressiveFanoutOpt (known unstable, W8A8-era crashed ~90%% through twice)..."
phys_opt_design -directive AggressiveFanoutOpt

set wns_after [get_property SLACK [get_timing_paths -max_paths 1 -delay_type max]]
puts ""
puts "========================================"
puts ">>> WNS after AggressiveFanoutOpt: $wns_after ns  (baseline: -2.562842ns; W8A8-era partial result before crash: -2.303ns)"
puts "========================================"

set ckpt_out "$proj_dir/fastvit_bd_wrapper_w8a4_aggressivefanout.dcp"
write_checkpoint -force $ckpt_out
puts ">>> Checkpoint saved: $ckpt_out"

report_timing -delay_type max -max_paths 5 -sort_by group \
    -file "$proj_dir/timing_detail_w8a4_aggressivefanout.rpt"
report_utilization -file "$proj_dir/utilization_w8a4_aggressivefanout.rpt"
puts ">>> Done."
