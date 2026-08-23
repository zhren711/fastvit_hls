# run_bitstream_mac_array_a3_final.tcl -- ZHR-92, 2026-08-23: bitstream for
# the final combined DW fix (DW_WT_STAGE gmem_w/gmem_b split + K revert,
# DW_PATCH_STAGE patch_r/patch_c revert) with the widened pblock
# (X0-120). route_design alone gave WNS=+0.113ns, no phys_opt needed.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/mac_array_a3_proj_final_wide
set dcp_in   "$proj_dir/mac_array_a3_proj_final_wide.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/mac_array_a3_proj_final_wide.runs/impl_1/mac_array_bd_wrapper_final.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
