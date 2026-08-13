# round6_congestion_phase1.tcl
# Cheap, parallel round-6 experiment: does a congestion-focused placement
# subdirective alone (no RTL/HLS change) reduce the route-delay-dominated
# WNS seen in round 5, on the EXACT SAME synthesized netlist?
#
# Round-5's signed-off timing_detail report showed the top-10 worst paths
# (8/10 touching pw_acc/WRITEBACK_PW) are dominated by ROUTE delay
# (56-84% of total data-path delay), not logic depth (only 2-4 LUT
# levels each). That, plus 77% LUT / 58% BRAM utilization on a small
# xc7z020, points at placement congestion rather than a single fixable
# logic hotspot. Round 5's place_design used the property directive
# "ExtraTimingOpt"; this tries a different directive instead on the SAME
# post-opt_design checkpoint, so any WNS delta is attributable purely to
# the placement strategy, not a netlist change.
# NOTE: -subdirective (GPlace.ReduceCongestion.high etc.) is UltraScale+
# only -- errored with "not supported for part xc7z020clg400-1" on the
# first attempt. Falling back to plain -directive Explore ("Increased
# placer effort in detail placement and post-placement optimization"),
# confirmed valid for this part via place_design -h.
#
# Reuses fastvit_bd_wrapper_opt.dcp from the round-5 run (synthesis
# unchanged -- this experiment does NOT touch dsconv_worker.cpp/h).

set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_dsconv_fused_only_200mhz_proj"
set run_dir    "$proj_dir/fastvit_dsconv_fused_only_200mhz_proj.runs/impl_1"
set opt_dcp    "$run_dir/fastvit_bd_wrapper_opt.dcp"

puts ">>> Opening post-opt_design checkpoint (round 5's, unchanged netlist)..."
open_checkpoint $opt_dcp

puts ">>> Running place_design -directive Explore..."
place_design -directive Explore

puts ">>> Writing placed checkpoint..."
write_checkpoint -force "$run_dir/fastvit_bd_wrapper_placed_round6cong.dcp"

puts ">>> Running pre-route phys_opt_design (AggressiveExplore, same as round 5)..."
phys_opt_design -directive AggressiveExplore

puts ">>> Writing physopt checkpoint..."
write_checkpoint -force "$run_dir/fastvit_bd_wrapper_physopt_round6cong.dcp"

puts ">>> Running route_design..."
route_design

puts ">>> Writing routed (pre-postopt) checkpoint..."
write_checkpoint -force "$run_dir/fastvit_bd_wrapper_routed_preopt_round6cong.dcp"

puts ">>> Phase 1 (congestion experiment) done."
