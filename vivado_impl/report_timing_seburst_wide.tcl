set proj_dir [file normalize "./seburstimp"]
open_project "$proj_dir/seburstimp.xpr"
open_run impl_1
set rpt "$proj_dir/timing_wide_seburst.rpt"
report_timing -max_paths 300 -nworst 1 -delay_type max -sort_by slack -file $rpt
puts ">>> $rpt"
