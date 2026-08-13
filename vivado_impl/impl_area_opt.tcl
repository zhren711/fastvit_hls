# impl_area_opt.tcl
# 对 pwconv_ip OOC 综合使用 AreaOptimized_high，强制减少 LUT
set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
open_project "$script_dir/fastvit_util_check/fastvit_util_check.xpr"

# 设置 pwconv OOC 综合指令为 AreaOptimized_high (最激进面积优化)
set pw_run [get_runs fastvit_bd_pwconv_ip_0_0_synth_1]
if {[llength $pw_run] > 0} {
    set_property STEPS.SYNTH_DESIGN.ARGS.DIRECTIVE AreaOptimized_high $pw_run
    puts ">>> Set pwconv OOC synth directive to AreaOptimized_high"
    # 强制重新综合 pwconv
    reset_run fastvit_bd_pwconv_ip_0_0_synth_1
    launch_runs fastvit_bd_pwconv_ip_0_0_synth_1 -jobs 4
    wait_on_run fastvit_bd_pwconv_ip_0_0_synth_1
    puts ">>> pwconv OOC synth done: [get_property STATUS $pw_run]"
} else {
    puts "WARNING: pwconv OOC synth run not found"
}

# 重置并运行 synth_1 + impl_1
reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synth status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap    [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true               [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraNetDelay_high [get_runs impl_1]

reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"

open_run impl_1
report_utilization -file "$script_dir/fastvit_util_check/utilization_area_opt.rpt"
puts ">>> BIT: $script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"
