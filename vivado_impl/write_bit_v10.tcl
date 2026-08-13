# Write bitstream from existing routed DCP (no re-implementation needed)
set dcp "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper_routed.dcp"
set bit "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"

puts ">>> Opening routed checkpoint..."
open_checkpoint $dcp

puts ">>> Writing bitstream..."
write_bitstream -force $bit

puts ">>> Done: $bit"
