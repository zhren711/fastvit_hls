# run_w8a4_200mhz_multicycle.tcl — safer alternative to Tier B's custom
# AXI mux (per user decision 2026-08-01: reduce risk by using
# time-division reuse via a multicycle path constraint, not hand-written
# AXI protocol logic). Reopens the existing fastvit_w8a4_200mhz_proj
# (synth_1 already completed), adds op_code_multicycle.xdc to the
# constraints fileset, resets and reruns impl_1 from scratch through
# the FULL flow (opt/place/route all see the relaxed timing requirement
# from the start, rather than bolting the constraint onto an
# already-1-cycle-optimized checkpoint).
#
# Baseline so far: -2.873ns (v1.2/W8A8) -> -2.563ns (W8A4 narrower
# datapath) -> -2.397ns (+ AggressiveFanoutOpt, converged after 1 round).
# This constraint targets the SAME class of path from a different angle
# (relax the timing requirement itself, since it's over-conservative by
# construction -- see op_code_multicycle.xdc for the safety reasoning).
#
# 用法: vivado -mode batch -source run_w8a4_200mhz_multicycle.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_w8a4_linebuf_200mhz_proj"
set proj_file  "$proj_dir/fastvit_w8a4_linebuf_200mhz_proj.xpr"

open_project $proj_file

puts ">>> Adding op_code_multicycle.xdc to constraints fileset..."
add_files -fileset constrs_1 -norecurse "$script_dir/op_code_multicycle.xdc"
import_files -fileset constrs_1 "$script_dir/op_code_multicycle.xdc"

puts ">>> Resetting impl_1 (reusing existing synth_1 netlist, full opt+place+route rerun)..."
reset_run impl_1

set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap       [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default              [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

open_run impl_1
set rpt_file "$proj_dir/utilization_w8a4_200mhz_multicycle.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_200mhz_multicycle.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (W8A4 v2.1, 200MHz, op_code multicycle)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns  (prior best: -2.397ns w/ AggressiveFanoutOpt; W8A8 baseline: -2.873ns)"
puts ">>> Bitstream: $proj_dir/fastvit_w8a4_linebuf_200mhz_proj.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
