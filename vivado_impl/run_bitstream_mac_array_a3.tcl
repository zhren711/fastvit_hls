# run_bitstream_mac_array_a3.tcl -- A3 bitstream generation (ZHR-92, 2026-08-21):
# second phase of the ZHR-17 two-phase recipe. Phase 1 (route_design only,
# no phys_opt_design) already ran and produced a MET routed checkpoint
# (WNS +0.244245ns, mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_routed.dcp).
# This script reopens that checkpoint and runs phys_opt_design SINGLE-THREADED
# (set_param general.maxThreads 1, per ZHR-17: phys_opt_design gets silently
# killed under foreground/multithreaded execution in this environment -- this
# is the proven two-phase fix), then write_bitstream. Approved by user
# 2026-08-21 following the clean route_design result.
#
# 用法: vivado -mode batch -source run_bitstream_mac_array_a3.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/mac_array_a3_proj"
set dcp_in     "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out    "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Running phys_opt_design (single-threaded, ZHR-17 recipe)..."
phys_opt_design

puts ">>> Writing bitstream..."
write_checkpoint -force "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_phys_opt.dcp"
write_bitstream -force $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
puts ""
puts "========================================"
puts " POST-PHYS_OPT_DESIGN TIMING + BITSTREAM"
puts "========================================"
report_utilization
report_timing -delay_type max -max_paths 5 -sort_by group \
    -file "$proj_dir/timing_detail_mac_array_a3_physopt.rpt"
puts ""
puts ">>> WNS (post phys_opt_design): $wns ns"
puts ">>> Bitstream: $bit_out"
