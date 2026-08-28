# run_bitstream_mac_array_a3_dwraster_step2_lut.tcl -- ZHR-92, 2026-08-28:
# bitstream for the DW raster integration (Step 2), LUT-mode binding, the
# ONLY real-P&R-passing build on this line so far (WNS=+0.021306ns,
# route_design alone, no phys_opt). Reopens the already-routed checkpoint
# from that exact run -- no re-synthesis, no re-P&R.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/a3dwr2lut
set dcp_in   "$proj_dir/a3dwr2lut.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/a3dwr2lut.runs/impl_1/mac_array_bd_wrapper_dwrasterstep2lut.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
