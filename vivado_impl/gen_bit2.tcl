set_param general.maxThreads 4
set dcp "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper_physopt.dcp"
set bit "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_bd_wrapper.bit"
set rpt "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/utilization_final.rpt"
open_checkpoint $dcp
write_bitstream -force $bit
puts ">>> Bitstream: $bit"
report_utilization -file $rpt
puts ">>> Done."
