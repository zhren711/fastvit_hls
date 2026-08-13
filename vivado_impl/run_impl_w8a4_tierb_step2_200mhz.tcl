# run_impl_w8a4_tierb_step2_200mhz.tcl -- Tier B Step 2 (gmem0/gmem3-private,
# gmem1/gmem2-shared) Block Design + synth + impl + bitstream at 200MHz.
#
# New middle ground between Tier A (4 shared masters, best -2.164~-2.397ns)
# and Tier B partial-independence (8 masters: dwconv/pwconv gmem0/1 private,
# everything else shared, combined WNS=-3.021ns). This session's fresh
# diagnosis on today's Tier A combined build found 8/10 worst paths on
# conv_worker's LOAD_IN reaching into the SHARED gmem0 adapter's internal
# request FIFO (serving all 5 op-codes) -- gmem0(feat_in)/gmem3(feat_out)
# are the two bundles every worker touches, carrying the densest
# mux/arbitration structure. This version makes gmem0 AND gmem3 private
# per-worker for ALL 5 workers (10 masters), while folding dwconv/pwconv's
# previously-private gmem1 INTO the shared gmem1 mux (now serving all 4
# weight-reading workers) and leaving gmem2 shared as before -- 12 physical
# masters total. LUT-budget arithmetic done before this run (measured from
# isolated single-worker gmem0+gmem3 adapter costs: conv 1843, add 2202,
# dwconv 2264, pwconv 2351, gelu ~2050 est. -> +8318 LUT vs today's shared
# adapter cost, projecting to ~93.6% utilization) said this was tight but
# worth trying for a real number. Re-verified via xsim regression (ALL
# TESTS PASSED, all 5 op-codes + ADD->GELU sequence) before this synth
# attempt. See project memory / plan at
# C:\Users\zhren\.claude\plans\vivid-gliding-lynx.md Step 2.
#
# 12 masters fit in a SINGLE SmartConnect (NUM_SI max is 16), so this
# version needs only sc_main -> HP0, no HP1 split (same as the 8-master
# lineage).
#
# 用法: vivado -mode batch -source run_impl_w8a4_tierb_step2_200mhz.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_w8a4_tierb_step2_200mhz_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_tierb "$script_dir/../fastvit_ip_w8a4/tier_b_rtl_ip_step2"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_tierb]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (单 HP0 口即可，只有12路master) ────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {200} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

# ── Tier B Step 2 顶层 (12 个 m_axi master: 10 私有 gmem0/gmem3 + 2 共享 gmem1/gmem2) ──
create_bd_cell -type ip -vlnv user.org:hls:fastvit_top_tierb:1.0 tierb_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_200M

# ── AXI-Lite 控制总线: PS GP0 → 2 Slaves (s_axi_control + s_axi_ctrl) ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 12 个 master → 1 个 SmartConnect → HP0 ──
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_main
set_property CONFIG.NUM_SI 12 [get_bd_cells sc_main]

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

# ── AXI 数据连接: 12 个 master (10私有gmem0/gmem3+2共享gmem1/gmem2) -> sc_main -> HP0 ──
set idx 0
foreach m {conv_gmem0 dwconv_gmem0 pwconv_gmem0 add_gmem0 gelu_gmem0 \
           conv_gmem3 dwconv_gmem3 pwconv_gmem3 add_gmem3 gelu_gmem3 \
           shared_gmem1 shared_gmem2} {
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

# ── Post-synth utilization check BEFORE spending impl time (this design's
#    LUT-budget arithmetic said ~93.6% -- worth confirming early) ──────────
open_run synth_1
puts ""
puts "========================================"
puts " POST-SYNTHESIS UTILIZATION (Step 2, 200MHz)"
puts "========================================"
report_utilization

# ── Implementation (同 8-master partial-independence 版本用的同一套
#    directive，便于公平对比) ──────────
set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap       [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraTimingOpt       [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

# ── Utilization + Timing Report ──────────────────────────
open_run impl_1
set rpt_file "$proj_dir/utilization_w8a4_tierb_step2_200mhz.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_tierb_step2_200mhz.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (Tier B Step 2, 200MHz)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
set tns [get_property STATS.TNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> TNS: $tns ns"
puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
