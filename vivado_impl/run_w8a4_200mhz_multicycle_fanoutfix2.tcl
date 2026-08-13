# run_w8a4_200mhz_multicycle_fanoutfix2.tcl — stacks a targeted fanout
# fix (round 2, pre_opt_fanout_w8a4_step2.tcl) on top of the already-
# applied op_code_multicycle.xdc constraint, on the SAME
# fastvit_w8a4_200mhz_proj used by run_w8a4_200mhz_multicycle.tcl
# (which reached -2.573ns with today's 2026-08-08 dwconv/pwconv source
# + Step1 ch_out_buf change + the multicycle constraint alone). This
# reopens that same project (op_code_multicycle.xdc is already in
# constrs_1 from the prior run, no need to re-add it), resets impl_1
# again, and reruns opt/place/route with the new OPT_DESIGN.TCL.PRE
# fanout hook targeting this session's real diagnosed worst cells
# (see pre_opt_fanout_w8a4_step2.tcl for the exact cells/rationale).
#
# Baseline going in: -2.573ns (multicycle only). Historical Tier A
# bests for comparison: -2.397ns (old dwconv/pwconv source + fanout
# fix only, no multicycle), -2.164ns (best on record, fanout fix +
# more accumulated tuning).
#
# 用法: vivado -mode batch -source run_w8a4_200mhz_multicycle_fanoutfix2.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_w8a4_200mhz_proj"
set proj_file  "$proj_dir/fastvit_w8a4_200mhz_proj.xpr"

open_project $proj_file

puts ">>> Resetting impl_1 (reusing existing synth_1 netlist + already-applied op_code_multicycle.xdc)..."
reset_run impl_1

set_property STEPS.OPT_DESIGN.TCL.PRE          [list "$script_dir/pre_opt_fanout_w8a4_step2.tcl"] [get_runs impl_1]
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
set rpt_file "$proj_dir/utilization_w8a4_200mhz_multicycle_fanoutfix2.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_200mhz_multicycle_fanoutfix2.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (W8A4 v2.1, 200MHz, multicycle + fanout fix round 2)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns  (baseline this session: -2.573ns multicycle-only; historical: -2.397ns / -2.164ns)"
puts ">>> Bitstream: $proj_dir/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
