set proj_dir [file normalize "./fsweep80imp"]
open_project "$proj_dir/fsweep80imp.xpr"
open_run impl_1
set rpt "$proj_dir/timing_wide_fsweep80.rpt"
report_timing -max_paths 300 -nworst 1 -delay_type max -sort_by slack -file $rpt
puts ">>> $rpt"
