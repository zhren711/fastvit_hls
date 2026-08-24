# run_bitstream_mac_array_a3_sol35_burstwriteout.tcl -- ZHR-92, 2026-08-24:
# bitstream for the WRITEOUT hls::burst_maxi dual-path round. X0-120
# pblock (same as the last successful PW-flat round). route_design alone
# gave WNS=+0.098407ns, no phys_opt needed.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/a3wp26
set dcp_in   "$proj_dir/a3wp26.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/a3wp26.runs/impl_1/mac_array_bd_wrapper_burstwriteout.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
