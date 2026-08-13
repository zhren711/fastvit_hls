# round6_congestion_phase2.tcl
# Phase 2 of the round-6 congestion/placement-directive experiment:
# resume from phase 1's routed (pre-postopt) checkpoint and run the SAME
# post-route phys_opt_design + signoff reports round 5 used, so the
# final comparison against round 5's WNS=-1.500ns is apples-to-apples.
# Single-threaded, same reason as round5_phase2_physopt.tcl (repeated
# silent kills under multithreaded phys_opt_design in this session).
set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_dsconv_fused_only_200mhz_proj"
set run_dir    "$proj_dir/fastvit_dsconv_fused_only_200mhz_proj.runs/impl_1"
set routed_preopt_dcp "$run_dir/fastvit_bd_wrapper_routed_preopt_round6cong.dcp"

puts ">>> Opening routed (pre-postopt) checkpoint..."
open_checkpoint $routed_preopt_dcp

puts ">>> Running post-route phys_opt_design (AggressiveExplore)..."
phys_opt_design -directive AggressiveExplore

puts ">>> Writing routed checkpoint..."
write_checkpoint -force "$run_dir/fastvit_bd_wrapper_routed_round6cong.dcp"

puts ">>> Timing summary..."
report_timing_summary -delay_type max -max_paths 20 -file "$proj_dir/timing_summary_dsconv_fused_round6cong.rpt"

puts ">>> Utilization..."
report_utilization -file "$proj_dir/utilization_dsconv_fused_round6cong.rpt"

puts ">>> Detailed worst-path timing..."
report_timing -delay_type max -max_paths 10 -sort_by group -file "$proj_dir/timing_detail_dsconv_fused_round6cong.rpt"

puts ">>> Done."
