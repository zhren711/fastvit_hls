# round5_phase1_route.tcl
# Phase 1 of a split round-5 resume: just get from the physopt checkpoint
# through route_design and save a checkpoint, so phase 2 (physopt, which
# keeps dying under Bash-tool background execution for an unknown reason)
# doesn't have to burn ~2:45 re-routing on every retry.
set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_dsconv_fused_only_200mhz_proj"
set run_dir    "$proj_dir/fastvit_dsconv_fused_only_200mhz_proj.runs/impl_1"
set physopt_dcp "$run_dir/fastvit_bd_wrapper_physopt.dcp"

puts ">>> Opening physopt checkpoint..."
open_checkpoint $physopt_dcp

puts ">>> Running route_design..."
route_design

puts ">>> Writing routed (pre-postopt) checkpoint..."
write_checkpoint -force "$run_dir/fastvit_bd_wrapper_routed_preopt_round5.dcp"

puts ">>> Phase 1 done."
