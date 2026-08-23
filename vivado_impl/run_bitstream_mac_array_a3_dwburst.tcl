# run_bitstream_mac_array_a3_dwburst.tcl -- ZHR-92, 2026-08-22: bitstream
# for the DW whole-block-burst design (widened pblock X0-120). WNS=+0.030ns
# after phys_opt_design (route_design alone was -0.012ns, essentially the
# noise floor -- phys_opt closed it cleanly, no congestion issues).
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/mac_array_a3_proj_dwburst_wide
set dcp_in   "$proj_dir/mac_array_a3_proj_dwburst_wide.runs/impl_1/mac_array_bd_wrapper_routed_physopt.dcp"
set bit_out  "$proj_dir/mac_array_a3_proj_dwburst_wide.runs/impl_1/mac_array_bd_wrapper_dwburst.bit"

puts ">>> Opening post-phys_opt checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
