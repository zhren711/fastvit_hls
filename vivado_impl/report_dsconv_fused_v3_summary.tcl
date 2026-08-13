set script_dir [file normalize [file dirname [info script]]]
set proj_dir "$script_dir/fastvit_dsconv_fused_only_200mhz_proj"
open_project "$proj_dir/fastvit_dsconv_fused_only_200mhz_proj.xpr"
open_run impl_1
report_timing_summary -delay_type max -max_paths 1 -file "$proj_dir/timing_summary_dsconv_fused_v3.rpt"
puts ">>> done"
