set proj_dir [file normalize "./preg100imp"]
open_project "$proj_dir/preg100imp.xpr"
open_run impl_1
set rpt "$proj_dir/timing_wide_preg100.rpt"
report_timing -max_paths 300 -nworst 1 -delay_type max -sort_by slack -file $rpt
puts ">>> $rpt"
