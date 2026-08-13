# run_impl_w8a4_200mhz_fanoutfix.tcl — Tier A of the Phase 3 200MHz plan:
# reopen the existing fastvit_w8a4_200mhz_proj (synth_1 already completed,
# WNS=-2.562842ns baseline), reset ONLY impl_1 (reusing synth_1's netlist
# so cell names are guaranteed identical to the diagnosed report_timing),
# apply a MAX_FANOUT constraint to the exact real bottleneck cell found
# in that run (gmem1_m_axi_U/.../mem_reg[5][0]_srl6_i_9, fanout=203, see
# pre_opt_fanout_w8a4.tcl), and re-run implementation.
#
# 用法: vivado -mode batch -source run_impl_w8a4_200mhz_fanoutfix.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/fastvit_w8a4_200mhz_proj"
set proj_file  "$proj_dir/fastvit_w8a4_200mhz_proj.xpr"

open_project $proj_file

puts ">>> Resetting impl_1 (reusing existing synth_1 netlist)..."
reset_run impl_1

set_property STEPS.OPT_DESIGN.TCL.PRE          [list "$script_dir/pre_opt_fanout_w8a4.tcl"] [get_runs impl_1]
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
set rpt_file "$proj_dir/utilization_w8a4_200mhz_fanoutfix.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_200mhz_fanoutfix.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (W8A4 v2.1, 200MHz, targeted fanout fix)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns  (baseline without fix: -2.562842ns; v1.2/W8A8 200MHz baseline: -2.873ns)"
puts ">>> Bitstream: $proj_dir/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
