# run_impl_v13.tcl — Upgrade dwconv_ip to v13.0 (FILM-QNN DSP packing), re-implement
#
# 步骤:
# 1. 打开 fastvit_util_check 项目
# 2. 更新 IP repo 路径 (dwconv_ip v13.0, 独立于 v12 项目目录)
# 3. 检测并升级 BD 中的 IP (dwconv_ip v12.0 → v13.0)
# 4. 重新生成 BD wrapper
# 5. 重跑 Synthesis + Implementation + Bitstream
#
# 前提: 先运行 vitis_hls -f run_hls_v13.tcl (生成 v13.0 IP export)
# 用法: vivado -mode batch -source run_impl_v13.tcl -nolog -nojournal

set script_dir [file normalize [file dirname [info script]]]
set_param general.maxThreads 4

# ── 打开已有项目 ───────────────────────────────────────────
set proj_xpr "$script_dir/fastvit_util_check/fastvit_util_check.xpr"
puts ">>> Opening project: $proj_xpr"
open_project $proj_xpr

# ── 更新 HLS IP repo 路径 (dwconv_ip 指向 v13 独立项目目录) ─
set ip_conv   "$script_dir/../conv_ip/conv_ip_proj/solution1/impl/ip"
set ip_dw     "$script_dir/../dwconv_ip/dwconv_ip_proj_v13/solution1/impl/ip"
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

puts ">>> Rebuilding IP catalog (picks up dwconv_ip v13.0)..."
update_ip_catalog -rebuild

# ── 升级 BD 中 dwconv_ip v12.0 → v13.0 ──────────────────
open_bd_design [get_files fastvit_bd.bd]

set upgrade_ips [get_ips -filter {UPGRADE_VERSIONS != ""}]
if {[llength $upgrade_ips] > 0} {
    puts ">>> Upgrading IPs: $upgrade_ips"
    upgrade_ip $upgrade_ips
    puts ">>> IP upgrade complete."
} else {
    puts ">>> No IPs need upgrading (check that v13.0 export is done)."
}

# 重新生成 BD output products
puts ">>> Regenerating BD output products..."
generate_target all [get_files fastvit_bd.bd]
save_bd_design

# 重新生成 HDL wrapper
set bd_file [get_files -of_objects [get_filesets sources_1] {*.bd}]
make_wrapper -files [list $bd_file] -top -force
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
puts ">>> BD wrapper regenerated."

# ── 重置并重跑 Synthesis ──────────────────────────────────
puts ">>> Resetting and launching Synthesis..."
reset_run synth_1
launch_runs synth_1 -jobs 4
wait_on_run synth_1
set synth_stat [get_property STATUS   [get_runs synth_1]]
set synth_prog [get_property PROGRESS [get_runs synth_1]]
puts ">>> Synthesis: $synth_stat  $synth_prog"
if {$synth_prog ne "100%"} { error "Synthesis FAILED: $synth_stat" }

# ── 重置并重跑 Implementation + Bitstream ────────────────
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap    [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true               [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default [get_runs impl_1]

puts ">>> Resetting and launching Implementation + Bitstream..."
reset_run impl_1
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
set impl_stat [get_property STATUS   [get_runs impl_1]]
set impl_prog [get_property PROGRESS [get_runs impl_1]]
puts ">>> Implementation: $impl_stat  $impl_prog"
if {$impl_prog ne "100%"} { error "Implementation FAILED: $impl_stat" }

# ── 利用率 + 时序报告 ────────────────────────────────────
open_run impl_1
set rpt_file "$script_dir/fastvit_util_check/utilization_v13.rpt"
report_utilization -file $rpt_file
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (v13) "
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Report:    $rpt_file"
puts ">>> Bitstream: [get_property DIRECTORY [get_runs impl_1]]/fastvit_bd_wrapper.bit"
