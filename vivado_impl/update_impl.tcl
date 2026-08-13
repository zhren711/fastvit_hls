# update_impl.tcl - 复用旧项目，更新HLS IP路径后重跑综合+实现
#
# 策略: 不新建项目，打开已有的 fastvit_util_check（含SmartConnect IP cache），
#       更新5个HLS IP的repo路径，刷新catalog，reset+rerun synth/impl。
#
# 用法: vivado -mode batch -source update_impl.tcl -nolog -nojournal

set script_dir [file normalize [file dirname [info script]]]

# ── 限制并行线程，避免 rodin slave 创建失败 ──────────────
set_param general.maxThreads 4

# (激进优化策略在 impl_1 run properties 里设置)

# ── 打开已有项目 ───────────────────────────────────────────
set proj_xpr "$script_dir/fastvit_util_check/fastvit_util_check.xpr"
puts ">>> Opening existing project: $proj_xpr"
open_project $proj_xpr

# ── 更新 HLS IP repo 路径 ─────────────────────────────────
set ip_conv   "$script_dir/../conv_ip/conv_ip_proj/solution1/impl/ip"
set ip_dw     "$script_dir/../dwconv_ip/dwconv_ip_proj/solution1/impl/ip"
set ip_pw     "$script_dir/../pwconv_ip/pwconv_ip_proj/solution1/impl/ip"
set ip_add    "$script_dir/../add_ip/add_ip_proj/solution1/impl/ip"
set ip_pool   "$script_dir/../pool_ip/pool_ip_proj/solution1/impl/ip"

set_property ip_repo_paths [list \
    [file normalize $ip_conv] \
    [file normalize $ip_dw]   \
    [file normalize $ip_pw]   \
    [file normalize $ip_add]  \
    [file normalize $ip_pool] \
] [current_project]

puts ">>> Updating IP catalog..."
update_ip_catalog -rebuild

# ── 检查 BD 中的 IP 版本是否需要升级 ──────────────────────
open_bd_design [get_files fastvit_bd.bd]
# 注: PS7 FCLK0 在 BD 内不能直接设为200MHz (需要PLL divider配置)
# 时序约束由 Vivado XDC 控制; ARM 驱动在启动时通过 SLCR 配置 200MHz

set upgrade_ips [get_ips -filter {UPGRADE_VERSIONS != ""}]
if {[llength $upgrade_ips] > 0} {
    puts ">>> Upgrading IPs: $upgrade_ips"
    upgrade_ip $upgrade_ips
}

# 重新生成 BD wrapper (gen 目录被清理后必须重建)
generate_target all [get_files fastvit_bd.bd]
save_bd_design

# 重新生成 HDL wrapper
set bd_file [get_files -of_objects [get_filesets sources_1] {*.bd}]
puts ">>> BD file: $bd_file"
make_wrapper -files [list $bd_file] -top -force
# wrapper 生成到 .gen 目录
set proj_dir [get_property DIRECTORY [current_project]]
set wrapper_search [glob -nocomplain "$proj_dir/*.gen/sources_1/bd/fastvit_bd/hdl/fastvit_bd_wrapper.v"]
if {[llength $wrapper_search] > 0} {
    set wrapper_file [lindex $wrapper_search 0]
    if {[llength [get_files $wrapper_file]] == 0} {
        add_files -norecurse $wrapper_file
    }
}
set_property top fastvit_bd_wrapper [current_fileset]
update_compile_order -fileset sources_1
puts ">>> BD wrapper regenerated"

# ── 重置并重跑 Synthesis ──────────────────────────────────
puts ">>> Resetting synth_1 run..."
reset_run synth_1
puts ">>> Launching Synthesis (jobs=4)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synthesis status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

# ── 重置并重跑 Implementation + Bitstream (合并为一步) ────
# 使用激进策略推进时序: phys_opt retiming
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap    [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true               [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default [get_runs impl_1]

puts ">>> Resetting impl_1 run..."
reset_run impl_1
puts ">>> Launching Implementation + Bitstream (AggressiveExplore retiming, jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
puts "Implementation progress: [get_property PROGRESS [get_runs impl_1]]"

# ── 利用率报告 ────────────────────────────────────────────
open_run impl_1
set rpt_file "$script_dir/fastvit_util_check/utilization_impl_v2.rpt"
report_utilization -file $rpt_file
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION"
puts "========================================"
report_utilization
puts ""
puts ">>> Report: $rpt_file"
puts ">>> Bitstream: [get_property DIRECTORY [get_runs impl_1]]/fastvit_bd_wrapper.bit"
