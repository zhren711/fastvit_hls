# run_physopt_dummy4th.tcl -- ZHR-92 (2026-09-05): single-threaded
# phys_opt_design on the dummy4th checkpoint (WNS=-0.133008ns after
# route_design alone), per this project's own established two-phase
# recipe (phys_opt_design gets silently killed under multithreaded
# foreground execution in this environment -- split into its own
# single-threaded batch invocation on a fresh checkpoint open).
set_param general.maxThreads 1

set proj_dir [file normalize "./dummy4thimp"]
open_project "$proj_dir/dummy4thimp.xpr"
open_run impl_1

puts ">>> Running phys_opt_design (single-threaded)..."
phys_opt_design

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
puts ""
puts ">>> WNS after phys_opt_design: $wns ns"

write_checkpoint -force "$proj_dir/dummy4thimp_physopt.dcp"
write_bitstream -force "$proj_dir/dummy4thimp_physopt.bit"
puts ">>> Bitstream: $proj_dir/dummy4thimp_physopt.bit"
