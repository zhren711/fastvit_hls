set proj_dir [file normalize "./flatimp"]
open_project "$proj_dir/flatimp.xpr"
open_run impl_1
set rpt "$proj_dir/timing_wide_flat.rpt"
report_timing -max_paths 300 -nworst 1 -delay_type max -sort_by slack -file $rpt
puts ">>> $rpt"
