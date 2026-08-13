# impl_with_good_pwconv.tcl
# 使用已替换的 Jun 21 好 DCP 重新运行 synth_1 + impl_1
# 注意: 不 reset OOC (fastvit_bd_pwconv_ip_0_0_synth_1), 让 Vivado 使用已有的好 DCP
set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
set proj_xpr "$script_dir/fastvit_util_check/fastvit_util_check.xpr"
open_project $proj_xpr

# 允许 LUT 超限 DRC 继续，让 opt_design 有机会优化
# (原工作 bitstream: opt_design 将 28K HLS LUT → 7.5K Vivado LUT)
set_param drc.disableLUTOverUtilError 1

# 只重置 synth_1 和 impl_1 (不重置 OOC IP 综合)
puts ">>> Resetting synth_1 and impl_1 only (keeping OOC DCP)..."
reset_run synth_1
reset_run impl_1

# 重新运行顶层综合 (使用已有 OOC DCP, 不重新综合 IP)
puts ">>> Launching synth_1..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
set synth_status [get_property STATUS [get_runs synth_1]]
puts "Synthesis status: $synth_status"
if {$synth_status ne "synth_design Complete!"} {
    error "Synthesis FAILED: $synth_status"
}

# 实现 + Bitstream
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap    [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true               [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraNetDelay_high [get_runs impl_1]

puts ">>> Launching impl_1 to write_bitstream..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"

open_run impl_1
report_utilization -file "$script_dir/fastvit_util_check/utilization_good_pwconv.rpt"
report_timing_summary -file "$script_dir/fastvit_util_check/timing_good_pwconv.rpt"

puts ">>> Bitstream: $script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"
