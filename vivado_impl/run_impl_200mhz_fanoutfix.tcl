# run_impl_200mhz_fanoutfix.tcl — FastVIT unified IP v1.6 (v1.2 topology,
# 4 shared m_axi masters, HLS re-scheduled for 5ns/200MHz) + a targeted
# MAX_FANOUT constraint on the op_code control register, applied via a
# STEPS.OPT_DESIGN.TCL.PRE hook (before opt_design runs, i.e. before
# placement) rather than as a post-route phys_opt_design patch.
#
# Background: the plain 200MHz attempt (v1.4, 2026-07-30) gave WNS=-2.873ns.
# report_timing on the top-10 worst paths showed EVERY one of them starting
# at an op_code_read_reg bit and ending inside a shared m_axi bundle's
# auto-generated FIFO adapter logic -- 75-84% of the delay was ROUTE delay
# (physical distance), not logic depth. Three placement-strategy experiments
# (default, phys_opt_design -directive AggressiveFanoutOpt [crashed twice,
# but recovered 0.57ns before crashing], place_design -directive
# AltSpreadLogic_high [made it worse, WNS -3.267ns]) all hit the same class
# of bottleneck. This script tries a 4th, structurally different approach:
# force early register replication via MAX_FANOUT so the placer can put a
# local copy of the op_code decode near each of the 4 shared masters'
# adapters, instead of one register driving all of them from wherever it
# happens to land.
#
# 用法: vivado -mode batch -source run_impl_200mhz_fanoutfix.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_unified_200mhz_fanout_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_fastvit "$script_dir/../fastvit_ip/fastvit_ip_proj_200mhz/solution1/impl/ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_fastvit]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (单 HP0 口，4路 shared master，FCLK0=200MHz) ────────
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {200} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

# ── 统一 HLS IP 核 v1.6 (v1.2 拓扑, 200MHz HLS 目标) ───────
create_bd_cell -type ip -vlnv user.org:hls:fastvit_ip:1.6 fastvit_ip_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

# ── AXI-Lite 控制总线: PS GP0 → 2 Slaves (s_axi_control + s_axi_ctrl) ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 4 masters (in_a/in_b/bias/out) → 1 SmartConnect → HP0 ──
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_main
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_main]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins fastvit_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
foreach i {00 01} {
    connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M${i}_ACLK]
}
connect_bd_net $clk [get_bd_pins sc_main/aclk]

# ── 复位连接 ──────────────────────────────────────────────
connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] \
               [get_bd_pins rst_ps7_0_100M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_100M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins fastvit_ip_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
foreach i {00 01} {
    connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M${i}_ARESETN]
}
connect_bd_net $rstn [get_bd_pins sc_main/aresetn]

# ── AXI-Lite 控制连接 ─────────────────────────────────────
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins fastvit_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI] [get_bd_intf_pins fastvit_ip_0/s_axi_ctrl]

# ── AXI 数据连接: 4 shared masters → HP0 ──────────────────
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem0] [get_bd_intf_pins sc_main/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem1] [get_bd_intf_pins sc_main/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem2] [get_bd_intf_pins sc_main/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem3] [get_bd_intf_pins sc_main/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_main/M00_AXI]          [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ── 地址分配 ─────────────────────────────────────────────
assign_bd_address

# ── Validate & Save ───────────────────────────────────────
validate_bd_design
save_bd_design

# ── 显式生成 BD IP output products (串行，避免 "rodin" 竞争) ─
generate_target all [get_files fastvit_bd.bd]

# ── HDL Wrapper ───────────────────────────────────────────
make_wrapper -files [get_files fastvit_bd.bd] -top
set wrapper_files [get_files -filter {NAME =~ *fastvit_bd_wrapper.v}]
if {[llength $wrapper_files] == 0} {
    set wrapper "$proj_dir/${proj_name}.gen/sources_1/bd/fastvit_bd/hdl/fastvit_bd_wrapper.v"
    add_files -norecurse $wrapper
}
set_property top fastvit_bd_wrapper [current_fileset]
update_compile_order -fileset sources_1

# ── Synthesis ─────────────────────────────────────────────
puts ">>> Launching Synthesis (jobs=4)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synthesis status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

# ── Implementation: MAX_FANOUT hook BEFORE opt_design, then the same
#    directives used for every prior unified-IP run (fair comparison) ──
set_property STEPS.OPT_DESIGN.TCL.PRE          [list "$script_dir/pre_opt_fanout.tcl"] [get_runs impl_1]
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap       [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default              [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

# ── Utilization + Timing Report ──────────────────────────
open_run impl_1
set rpt_file "$proj_dir/utilization_200mhz_fanoutfix.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_200mhz_fanoutfix.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (v1.6, 200MHz, MAX_FANOUT fix)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns  (baseline without fix was -2.873ns)"
puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
