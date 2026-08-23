# run_bitstream_mac_array_a3_sol19_merge.tcl -- ZHR-92, 2026-08-23: bitstream
# for the PW_PATCH_HOIST wide/narrow merge round. route_design alone gave
# WNS=+0.115ns, no phys_opt needed.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/a3wp19
set dcp_in   "$proj_dir/a3wp19.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/a3wp19.runs/impl_1/mac_array_bd_wrapper_merge.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
