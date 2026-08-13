# resume_synth2.tcl - Resume top-level synthesis, clean approach
# All sub-module .dcp already exist from yesterday. We just need the top-level wrapper synth.
# Strategy: open project, mark sub-module synths as "complete" using their existing DCPs,
# then run only synth_1 (top-level wrapper).
# Usage: vivado -mode batch -source resume_synth2.tcl -nolog -nojournal

set script_dir [file normalize [file dirname [info script]]]
set proj_file  "$script_dir/fastvit_util_check/fastvit_util_check.xpr"

puts ">>> Opening project: $proj_file"
open_project $proj_file

# Report current run statuses
foreach r [get_runs] {
    puts "  Run: $r  STATUS=[get_property STATUS [get_runs $r]]  PROGRESS=[get_property PROGRESS [get_runs $r]]"
}

# Reset ONLY synth_1 (top-level), preserving sub-module run results
# -noclean_dir keeps existing DCP files
puts ">>> Resetting synth_1 only (preserving sub-module DCPs)..."
reset_run synth_1 -noclean_dir

# Verify sub-module runs are still complete
set sub_runs [list \
    fastvit_bd_conv_ip_0_0_synth_1 \
    fastvit_bd_dwconv_ip_0_0_synth_1 \
    fastvit_bd_pwconv_ip_0_0_synth_1 \
    fastvit_bd_add_ip_0_0_synth_1 \
    fastvit_bd_pool_ip_0_0_synth_1 \
    fastvit_bd_ps7_0_0_synth_1 \
    fastvit_bd_rst_ps7_0_100M_0_synth_1 \
    fastvit_bd_ps_ctrl_ic_imp_auto_pc_0_synth_1 \
    fastvit_bd_ps_ctrl_ic_imp_xbar_0_synth_1 \
    fastvit_bd_hp0_sc_a_0_synth_1 \
    fastvit_bd_hp0_sc_b_0_synth_1 \
    fastvit_bd_hp0_sc_top_0_synth_1 \
]

puts ">>> Sub-module run statuses after reset_run synth_1:"
foreach r $sub_runs {
    set runs [get_runs $r -quiet]
    if {$runs ne ""} {
        puts "  $r: STATUS=[get_property STATUS [get_runs $r]] PROGRESS=[get_property PROGRESS [get_runs $r]]"
    }
}

puts ">>> Launching synth_1 only (deps should be cached)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1

set stat [get_property STATUS [get_runs synth_1]]
set prog [get_property PROGRESS [get_runs synth_1]]
puts ">>> Synthesis status: $stat  progress: $prog"

if {$prog ne "100%"} {
    puts "ERROR: Synthesis FAILED: $stat"
    exit 1
}

puts ">>> Synthesis complete!"

# Utilization from synth
open_run synth_1
report_utilization
set rpt "$script_dir/fastvit_util_check/utilization_synth.rpt"
report_utilization -file $rpt
puts ">>> Synth utilization saved to: $rpt"

# Implementation
puts ">>> Launching Implementation..."
launch_runs impl_1 -jobs 4
wait_on_run impl_1

set istat [get_property STATUS [get_runs impl_1]]
set iprog [get_property PROGRESS [get_runs impl_1]]
puts ">>> Implementation: $istat  $iprog"

if {$iprog ne "100%"} {
    puts "ERROR: Implementation FAILED: $istat"
    exit 1
}

# Bitstream
puts ">>> Generating Bitstream..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1

# Final report
open_run impl_1
set rpt2 "$script_dir/fastvit_util_check/utilization_impl.rpt"
report_utilization -file $rpt2
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (5-IP) "
puts "========================================"
report_utilization
puts ""
puts ">>> Bitstream: $script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt2"
