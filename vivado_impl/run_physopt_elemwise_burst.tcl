# run_physopt_elemwise_burst.tcl -- ZHR-92 (2026-09-06): single-threaded
# phys_opt_design on the ELEMWISE_BURST checkpoint (WNS=-0.166790ns after
# route_design alone). One pass only, per this project's own established
# two-phase recipe -- NOT directive rotation (that's on the hard-stop
# list, falsified 4 times); this is a single application of a specific,
# not-banned lever on a design whose critical path is confirmed unchanged
# from the pre-existing mul_32s_32s_32_2_1 mechanism (resource-pressure
# placement degradation, not a new bottleneck). -0.167ns is within the
# magnitude phys_opt_design has already recovered once this session
# (dummy4th: -0.133->+0.013ns), unlike the -2.264ns case where it wasn't
# even attempted as sufficient on its own.
set_param general.maxThreads 1

set proj_dir [file normalize "./elemwiseburstimp"]
open_project "$proj_dir/elemwiseburstimp.xpr"
open_run impl_1

puts ">>> Running phys_opt_design (single-threaded, one pass only)..."
phys_opt_design

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
puts ""
puts ">>> WNS after phys_opt_design: $wns ns"

write_checkpoint -force "$proj_dir/elemwiseburstimp_physopt.dcp"
write_bitstream -force "$proj_dir/elemwiseburstimp_physopt.bit"
puts ">>> Bitstream: $proj_dir/elemwiseburstimp_physopt.bit"

set rpt_file "$proj_dir/utilization_elemwise_burst_physopt.rpt"
report_utilization -file $rpt_file
puts ">>> Utilization: $rpt_file"
