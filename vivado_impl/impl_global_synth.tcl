# impl_global_synth.tcl
# 切换到 Global (平坦) 综合模式，让 Vivado 跨模块优化消除 pwconv stall 逻辑
set_param general.maxThreads 4

set script_dir [file normalize [file dirname [info script]]]
open_project "$script_dir/fastvit_util_check/fastvit_util_check.xpr"

# 禁用各 IP 的 OOC 综合检查点，改为 Global 综合
# (Vivado 将所有 RTL 一起综合，允许跨模块常量传播)
open_bd_design [get_files fastvit_bd.bd]
foreach ip [get_ips] {
    set xci [get_files -of_objects $ip -filter {FILE_TYPE == IP}]
    if {[llength $xci] > 0} {
        puts "Setting $ip to global synthesis..."
        set_property GENERATE_SYNTH_CHECKPOINT 0 $ip
    }
}
save_bd_design
close_bd_design [get_bd_designs fastvit_bd]

# synth_1: 全局综合（无 OOC 子运行）
puts ">>> Resetting synth_1..."
reset_run synth_1

# 使用激进面积优化
set_property STEPS.SYNTH_DESIGN.ARGS.DIRECTIVE AreaOptimized_high [get_runs synth_1]
set_property STEPS.SYNTH_DESIGN.ARGS.FLATTEN_HIERARCHY rebuilt [get_runs synth_1]

puts ">>> Launching global synthesis..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synth status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

# 实现
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap    [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true               [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraNetDelay_high [get_runs impl_1]

reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation: [get_property STATUS [get_runs impl_1]]"

open_run impl_1
report_utilization -file "$script_dir/fastvit_util_check/utilization_global_synth.rpt"
puts ">>> BIT: $script_dir/fastvit_util_check/fastvit_util_check.runs/impl_1/fastvit_bd_wrapper.bit"
