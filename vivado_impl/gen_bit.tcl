set_param general.maxThreads 4
open_checkpoint {/e/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper_physopt.dcp}
write_bitstream -force {/e/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_bd_wrapper.bit}
puts ">>> Bitstream: /e/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_bd_wrapper.bit"
report_utilization -file {/e/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/utilization_final.rpt}
puts ">>> Utilization: /e/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/utilization_final.rpt"
