# run_impl_w8a4_tierb_pi_200mhz.tcl -- Tier B PARTIAL-INDEPENDENCE
# (hand-written top level) Block Design + synth + impl + bitstream at 200MHz.
#
# Follow-up to the full-independence attempt (run_impl_w8a4_tierb_200mhz.tcl,
# 17 masters): that version's LUT-metadata bug was found and fixed
# (READ_WRITE_MODE/HAS_BURST busif params), but it then hit a second,
# architectural blocker -- 1,981 distinct control sets (7-series has no
# control_set_opt) needing 8,021 slices vs 5,940 available, driven by the
# 17 independently-instantiated AXI adapters' own FIFOs/FSMs (59% of all
# control sets). This version rewrites fastvit_top_tierb.v to keep ONLY
# dwconv/pwconv's gmem0(feat_in)/gmem1(weight) private (4 masters -- the
# bundles implicated in the original Tier A adapter-cross-talk finding),
# and consolidates everything else (conv's all 4 bundles, add's gmem0/1/3,
# gelu's gmem0/3, dwconv/pwconv's own gmem2/3) onto 4 shared adapters (one
# per gmem ROLE, matching the working W8A4 100MHz baseline's shape), muxed
# by the existing independently-registered en_* dispatch signals -- 8
# physical masters total, down from 17. Re-verified via xsim regression
# (ALL TESTS PASSED) before this synth attempt. See project memory / plan
# at C:\Users\zhren\.claude\plans\typed-knitting-nygaard.md.
#
# 8 masters fit in a SINGLE SmartConnect (NUM_SI max is 16), so this
# version needs only sc_main -> HP0, no HP1 split.
#
# 用法: vivado -mode batch -source run_impl_w8a4_tierb_pi_200mhz.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_w8a4_tierb_pi_200mhz_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_tierb "$script_dir/../fastvit_ip_w8a4/tier_b_rtl_ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_tierb]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (单 HP0 口即可，只有8路master) ────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {200} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

# ── Tier B 顶层 (8 个 m_axi master: 4 私有 + 4 共享, 手写 dispatch) ──
create_bd_cell -type ip -vlnv user.org:hls:fastvit_top_tierb:1.0 tierb_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_200M

# ── AXI-Lite 控制总线: PS GP0 → 2 Slaves (s_axi_control + s_axi_ctrl) ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 8 个 master → 1 个 SmartConnect → HP0 ──
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_main
set_property CONFIG.NUM_SI 8 [get_bd_cells sc_main]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_200M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins tierb_0/ap_clk]
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
               [get_bd_pins rst_ps7_0_200M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_200M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins tierb_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
foreach i {00 01} {
    connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M${i}_ARESETN]
}
connect_bd_net $rstn [get_bd_pins sc_main/aresetn]

# ── AXI-Lite 控制连接 ─────────────────────────────────────
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins tierb_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI] [get_bd_intf_pins tierb_0/s_axi_ctrl]

# ── AXI 数据连接: 8 个 master (4私有+4共享) -> sc_main -> HP0 ──
set idx 0
foreach m {dwconv_gmem0 dwconv_gmem1 pwconv_gmem0 pwconv_gmem1 \
           shared_gmem0 shared_gmem1 shared_gmem2 shared_gmem3} {
    set si [format "S%02d_AXI" $idx]
    connect_bd_intf_net [get_bd_intf_pins tierb_0/m_axi_$m] [get_bd_intf_pins sc_main/$si]
    incr idx
}
connect_bd_intf_net [get_bd_intf_pins sc_main/M00_AXI] [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ── 地址分配 ─────────────────────────────────────────────
assign_bd_address

# ── Validate & Save ───────────────────────────────────────
validate_bd_design
save_bd_design

# ── 显式生成 BD IP output products (串行，避免 launch_runs 内部隐式并行生成
#    触发 "Could not create slave interpreter 'rodin'" 竞争错误) ──────────
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

# ── Implementation (同 Tier A 200MHz 实验用的 directive，便于公平对比) ──
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap       [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraTimingOpt       [get_runs impl_1]
# 2026-08-07: same placement-level fix validated on all 4 isolated
# workers this session (typed-knitting-nygaard.md) -- post-route
# phys_opt_design sees actual routed delays, the standard lever once a
# path is route-delay-dominated. Applying here for the first real
# combined-design attempt since the functional dwconv_start_hold fix
# (this rebuild is also the first combined run with a FUNCTIONALLY
# CORRECT dwconv path -- the original -14.264ns baseline predates that
# fix and every isolated-worker WNS number this session was measured on
# RTL that, for dwconv specifically, never actually completed an
# operation in simulation).
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

# ── Utilization + Timing Report ──────────────────────────
open_run impl_1
set rpt_file "$proj_dir/utilization_w8a4_tierb_pi_200mhz.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_tierb_pi_200mhz.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (Tier B, 200MHz)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
