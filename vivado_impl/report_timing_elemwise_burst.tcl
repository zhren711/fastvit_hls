open_project "elemwiseburstimp/elemwiseburstimp.xpr"
open_run impl_1
set timing_rpt "elemwiseburstimp/timing_detail_elemwise_burst.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
