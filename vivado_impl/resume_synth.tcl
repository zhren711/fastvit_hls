# resume_synth.tcl - Resume top-level synthesis (sub-module .dcp already cached)
# Usage: vivado -mode batch -source resume_synth.tcl -nolog -nojournal
# All 12 sub-module synth runs are already complete; this picks up where we left off.

set script_dir [file normalize [file dirname [info script]]]
set proj_file  "$script_dir/fastvit_util_check/fastvit_util_check.xpr"

puts ">>> Opening existing project: $proj_file"
open_project $proj_file

# Reset synth_1 status so Vivado will re-run it (sub-modules are cached and won't re-run)
reset_run synth_1

puts ">>> Launching top-level synth_1 (sub-modules already cached)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1

set stat [get_property STATUS [get_runs synth_1]]
set prog [get_property PROGRESS [get_runs synth_1]]
puts "Synthesis status: $stat  progress: $prog"

if {$prog != "100%"} {
    error "Synthesis FAILED: $stat"
}

puts ">>> Synthesis complete. Launching Implementation..."
launch_runs impl_1 -jobs 4
wait_on_run impl_1

set istat [get_property STATUS [get_runs impl_1]]
set iprog [get_property PROGRESS [get_runs impl_1]]
puts "Implementation status: $istat  progress: $iprog"

if {$iprog != "100%"} {
    error "Implementation FAILED: $istat"
}

puts ">>> Generating Bitstream..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1

# Utilization report
open_run impl_1
set rpt_file "$script_dir/fastvit_util_check/utilization_impl.rpt"
report_utilization -file $rpt_file
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (5-IP) "
puts "========================================"
report_utilization
puts ""
puts ">>> Bitstream: $script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
