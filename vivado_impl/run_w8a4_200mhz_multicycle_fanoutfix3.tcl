# run_w8a4_200mhz_multicycle_fanoutfix3.tcl — round 3 of the same idea:
# round 2 (fanoutfix2.tcl + pre_opt_fanout_w8a4_step2.tcl, OPT_DESIGN.
# TCL.PRE hook) produced a byte-identical WNS to the unfixed baseline
# (-2.573282ns both times) because the hook searched `get_cells` for
# net names, and separately because those net names don't exist until
# AFTER opt_design restructures the netlist (confirmed via
# diag_check_synth_stage.tcl: NOT FOUND on the raw synth_1 checkpoint).
# This round uses STEPS.OPT_DESIGN.TCL.POST instead (post_opt_fanout_
# w8a4_step2.tcl), which sets MAX_FANOUT=16 on the real nets (confirmed
# present with matching fanout ~270-289 on the post-opt checkpoint) and
# explicitly re-invokes `opt_design -fanout_opt` so the replication
# actually happens before place_design runs.
#
# Baseline going in: -2.573ns (multicycle only, this session's fair
# combined-IP number after today's dwconv/pwconv source changes).
# Historical Tier A bests for comparison: -2.397ns, -2.164ns.
#
# 用法: vivado -mode batch -source run_w8a4_200mhz_multicycle_fanoutfix3.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_w8a4_200mhz_proj"
set proj_file  "$proj_dir/fastvit_w8a4_200mhz_proj.xpr"

open_project $proj_file

puts ">>> Resetting impl_1 (reusing existing synth_1 netlist + already-applied op_code_multicycle.xdc)..."
reset_run impl_1

set_property STEPS.OPT_DESIGN.TCL.POST         [list "$script_dir/post_opt_fanout_w8a4_step2.tcl"] [get_runs impl_1]
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap         [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                    [get_runs impl_1]
# AggressiveFanoutOpt (not AggressiveExplore this round): a directive
# built specifically for fanout-driven replication/buffering, meant to
# actually consume the MAX_FANOUT property set on the two gmem push_0
# nets above -- opt_design has no -fanout_opt flag in this Vivado
# version (2024.2), so this is the real mechanism, not a manual call.
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveFanoutOpt [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default                [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

open_run impl_1
set rpt_file "$proj_dir/utilization_w8a4_200mhz_multicycle_fanoutfix3.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_200mhz_multicycle_fanoutfix3.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (W8A4 v2.1, 200MHz, multicycle + fanout fix round 3/POST hook)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns  (baseline this session: -2.573ns multicycle-only, round2 no-op; historical: -2.397ns / -2.164ns)"
puts ">>> Bitstream: $proj_dir/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
