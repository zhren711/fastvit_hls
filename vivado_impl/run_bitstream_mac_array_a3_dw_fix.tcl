# run_bitstream_mac_array_a3_dw_fix.tcl -- ZHR-92, 2026-08-22: bitstream for
# this round's combined fix (DW loop-bound zero-fill + pw_weight_cache
# revert), built from solution3 (confirmed-fresh RTL, see CLAUDE.md note on
# export_design's stale-HDL trap). Opens the already-phys_opt'd checkpoint
# (WNS=+0.120ns; phys_opt_design found zero violations, so this checkpoint
# IS the phys_opt result, not just the raw route) and writes the bitstream
# directly -- no need to re-run phys_opt_design.
#
# 用法: vivado -mode batch -source run_bitstream_mac_array_a3_dw_fix.tcl -nolog -nojournal

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/mac_array_a3_proj"
set dcp_in     "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_routed_physopt.dcp"
set bit_out    "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_dw_fix.bit"

puts ">>> Opening post-phys_opt checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream..."
write_bitstream -force $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
