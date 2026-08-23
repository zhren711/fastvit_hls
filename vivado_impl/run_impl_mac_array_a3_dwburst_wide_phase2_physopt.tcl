# run_impl_mac_array_a3_dwburst_wide_phase2_physopt.tcl -- ZHR-92,
# 2026-08-22: phase 2 of the ZHR-17 two-phase recipe for the DW
# whole-block-burst + widened-pblock build. route_design alone gave
# WNS=-0.012ns (essentially the noise floor) -- checking whether
# phys_opt_design closes it cleanly.
set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/mac_array_a3_proj_dwburst_wide"
set run_dir    "$proj_dir/mac_array_a3_proj_dwburst_wide.runs/impl_1"
set routed_dcp "$run_dir/mac_array_bd_wrapper_routed.dcp"

puts ">>> Opening routed checkpoint..."
open_checkpoint $routed_dcp

puts ">>> Running post-route phys_opt_design (AggressiveExplore)..."
phys_opt_design -directive AggressiveExplore

puts ">>> Writing post-phys_opt checkpoint..."
write_checkpoint -force "$run_dir/mac_array_bd_wrapper_routed_physopt.dcp"

puts ">>> Utilization..."
report_utilization -file "$proj_dir/utilization_mac_array_a3_physopt.rpt"

puts ">>> Detailed worst-path timing..."
report_timing -delay_type max -max_paths 10 -sort_by group -file "$proj_dir/timing_detail_mac_array_a3_physopt.rpt"

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> POST-PHYS_OPT WNS: $wns ns"
puts ">>> Done."
