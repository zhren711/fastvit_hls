# run_impl_w8a4_tierb_step2_200mhz_resume.tcl -- resume from the already-
# completed synth_1 (the first attempt got to synth_1 Complete, then died
# on a bug in MY OWN tcl script -- `get_property SLICE_LUTS [get_utilization]`
# is not a valid Vivado API call -- not a design failure). Reopens the
# existing project and proceeds straight to impl_1 without re-running
# synth (saves ~5 min of BD+synth time).
#
# Leaked utilization from the crash's error text (informative even though
# the script died): post-SYNTH (pre-place/pre-opt) LUT = 52,816/53,200 =
# 99.28% -- much tighter than the pre-P&R estimate of ~93.6%, consistent
# with this project's repeated pattern of isolated-component estimates
# undershooting the real combined-design cost. opt_design/place_design
# may still shave this down via resource sharing, or may fail outright
# like the historical 17-master Tier B attempt (1,981 control sets vs
# 5,940 available slices) -- this run finds out which for real.
#
# 用法: vivado -mode batch -source run_impl_w8a4_tierb_step2_200mhz_resume.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_name "fastvit_w8a4_tierb_step2_200mhz_proj"
set proj_dir   "$script_dir/$proj_name"

open_project "$proj_dir/${proj_name}.xpr"

puts "Synthesis status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "synth_1 did not complete previously -- cannot resume" }

open_run synth_1
puts ""
puts "========================================"
puts " POST-SYNTHESIS UTILIZATION (Step 2, 200MHz)"
puts "========================================"
report_utilization

# ── Implementation (同 8-master partial-independence 版本用的同一套
#    directive，便于公平对比) ──────────
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap       [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraTimingOpt       [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} {
    puts "!!! Implementation did NOT reach 100% -- likely failed (e.g. placement infeasible due to LUT/control-set budget)."
    puts "!!! Check $proj_dir/${proj_name}.runs/impl_1/runme.log for the real error."
} else {
    # ── Utilization + Timing Report ──────────────────────────
    open_run impl_1
    set rpt_file "$proj_dir/utilization_w8a4_tierb_step2_200mhz.rpt"
    report_utilization -file $rpt_file

    set timing_rpt "$proj_dir/timing_detail_w8a4_tierb_step2_200mhz.rpt"
    report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
    puts ">>> Detailed timing report: $timing_rpt"
    puts ""
    puts "========================================"
    puts " POST-IMPLEMENTATION UTILIZATION (Tier B Step 2, 200MHz)"
    puts "========================================"
    report_utilization
    set wns [get_property STATS.WNS [get_runs impl_1]]
    set tns [get_property STATS.TNS [get_runs impl_1]]
    puts ""
    puts ">>> WNS: $wns ns"
    puts ">>> TNS: $tns ns"
    puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
    puts ">>> Report:    $rpt_file"
}
