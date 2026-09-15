# report_timing_sohoist_wide.tcl -- ZHR-92 (2026-09-12, analysis round): what
# sits BEHIND the shared-multiplier sink? Top 300 paths, 1 per endpoint,
# so the next-worst DISTINCT structures and their slack bound the gain
# available from removing the sink's remaining mux arms.
set proj_dir [file normalize "./pwpfimp"]
open_project "$proj_dir/pwpfimp.xpr"
open_run impl_1
set rpt "$proj_dir/timing_wide_pwpf.rpt"
report_timing -max_paths 300 -nworst 1 -delay_type max -sort_by slack -file $rpt
puts ">>> $rpt"
