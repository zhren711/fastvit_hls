set_param general.maxThreads 4
open_project fastvit_util_check/fastvit_util_check.xpr
open_run impl_1
write_bitstream -force fastvit_util_check/fastvit_bd_wrapper.bit
puts ">>> Bitstream written: fastvit_util_check/fastvit_bd_wrapper.bit"
report_utilization -file fastvit_util_check/utilization_final.rpt
puts ">>> Utilization report: fastvit_util_check/utilization_final.rpt"
