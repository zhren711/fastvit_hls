# round5_phase2_physopt.tcl
# Phase 2 of a split round-5 resume: resume from the post-route checkpoint
# (phase 1) and run just phys_opt_design + signoff reports. Single-threaded
# (maxThreads=1) to rule out a threading-related kill trigger -- 6
# consecutive multithreaded attempts at this exact phase were killed
# cleanly (no crash log, empty stderr) shortly after entering the
# multithreaded physical-synthesis task.
set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_dsconv_fused_only_200mhz_proj"
set run_dir    "$proj_dir/fastvit_dsconv_fused_only_200mhz_proj.runs/impl_1"
set routed_preopt_dcp "$run_dir/fastvit_bd_wrapper_routed_preopt_round5.dcp"

puts ">>> Opening routed (pre-postopt) checkpoint..."
open_checkpoint $routed_preopt_dcp

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
