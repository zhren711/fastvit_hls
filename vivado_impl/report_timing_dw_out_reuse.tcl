# report_timing_dw_out_reuse.tcl -- ZHR-92 (2026-09-12): check critical
# path location BEFORE running phys_opt_design, per this round's own
# judgment criteria item 4 -- expect mul_32s_32s_32_2_1 (the pre-existing
# shared-multiplier mechanism, same as every prior thin-margin build on
# this line); if it moved to new DW code, that's new information.
set proj_dir [file normalize "./dwoutreuseimp"]
open_project "$proj_dir/dwoutreuseimp.xpr"
open_run impl_1

set rpt_file "$proj_dir/timing_detail_dw_out_reuse.rpt"
report_timing -max_paths 10 -nworst 1 -delay_type max -sort_by group -file $rpt_file
puts ">>> Timing report: $rpt_file"
