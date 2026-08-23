# run_bitstream_mac_array_a3_sol21_tight.tcl -- ZHR-92, 2026-08-23: bitstream
# for the PW_STAGE-elimination + accumulator-rewrite + pblock-tighten round.
# route_design alone gave WNS=+0.112ns (X0-96 pblock), no phys_opt needed.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/a3wp21b
set dcp_in   "$proj_dir/a3wp21b.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/a3wp21b.runs/impl_1/mac_array_bd_wrapper_pwstage.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
