# resume_from_physopt.tcl
# 从 physopt checkpoint 恢复并完成 route + write_bitstream
# (避免重新跑 opt_design 和 place_design，节省时间)
set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
set physopt_dcp "$script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper_physopt.dcp"
set bit_out "$script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"
set rpt_file "$script_dir/fastvit_util_check/utilization_v6.rpt"

puts ">>> Opening physopt checkpoint..."
open_checkpoint $physopt_dcp

puts ">>> Running route_design..."
route_design -directive Default

puts ">>> Timing summary..."
report_timing_summary -max_paths 10 -file "$script_dir/fastvit_util_check/timing_v6.rpt"

puts ">>> Utilization..."
report_utilization -file $rpt_file

puts ">>> Writing bitstream..."
write_bitstream -force $bit_out
puts ">>> Bitstream: $bit_out"
puts ">>> Done."
