# run_bitstream_mac_array_a3_pblock.tcl -- ZHR-92, 2026-08-22: bitstream for
# the pblock-fixed build (four-site loop-bound unification + pblock
# constraining mac_array_top_0 to a compact region). route_design alone
# gave WNS=+0.165ns with the critical path off gmem_meta -- no phys_opt
# needed, writing directly from the routed checkpoint.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/mac_array_a3_proj_pblock
set dcp_in   "$proj_dir/mac_array_a3_proj_pblock.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/mac_array_a3_proj_pblock.runs/impl_1/mac_array_bd_wrapper_pblock.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
