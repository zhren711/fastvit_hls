# resume_round5_from_physopt.tcl
# Round 5 (WRITEBACK_PW lr*pc hoist fix) got interrupted mid-flow last
# session right after route_design completed (routing itself succeeded,
# estimated WNS=-1.830/TNS=-11817 seen in the log) but before
# report_timing_summary / write_checkpoint / write_bitstream ran, so no
# signed-off numbers or routed checkpoint were saved. Resume from the
# already-computed physopt checkpoint (avoids re-running synth/opt/place,
# which are already done and cached) straight through route_design ->
# post-route phys_opt -> full signoff reports, matching every prior
# round's directive recipe exactly for a fair comparison.
set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_dsconv_fused_only_200mhz_proj"
set run_dir    "$proj_dir/fastvit_dsconv_fused_only_200mhz_proj.runs/impl_1"
set physopt_dcp "$run_dir/fastvit_bd_wrapper_physopt.dcp"

puts ">>> Opening physopt checkpoint..."
open_checkpoint $physopt_dcp

puts ">>> Running route_design..."
route_design

puts ">>> Running post-route phys_opt_design (AggressiveExplore)..."
phys_opt_design -directive AggressiveExplore

puts ">>> Writing routed checkpoint..."
write_checkpoint -force "$run_dir/fastvit_bd_wrapper_routed_round5.dcp"

puts ">>> Timing summary..."
report_timing_summary -delay_type max -max_paths 20 -file "$proj_dir/timing_summary_dsconv_fused_round5.rpt"

puts ">>> Utilization..."
report_utilization -file "$proj_dir/utilization_dsconv_fused_round5.rpt"

puts ">>> Detailed worst-path timing..."
report_timing -delay_type max -max_paths 10 -sort_by group -file "$proj_dir/timing_detail_dsconv_fused_round5.rpt"

puts ">>> Done."
