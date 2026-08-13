# run_impl_w8a4_tierb_200mhz.tcl -- Tier B (hand-written top level) Block
# Design + synth + impl + bitstream at 200MHz.
#
# Uses fastvit_top_tierb (packaged via package_tierb_ip.tcl,
# user.org:hls:fastvit_top_tierb:1.0) instead of the HLS-generated
# fastvit_ip: 4 real HLS black-box workers (conv/dwconv/pwconv/add) + 1
# hand-written GELU FSM, each with its OWN private AXI4 master ports (17
# total, no shared bundle/mux) instead of 4 masters shared+muxed by
# HLS-generated glue. That HLS-generated glue (op_code broadcast + shared
# burst-adapter cross-talk) was the confirmed 200MHz bottleneck across all
# Tier A attempts (WNS floor -2.16~-2.87ns) -- see project memory / plan at
# C:\Users\zhren\.claude\plans\typed-knitting-nygaard.md.
#
# SmartConnect's NUM_SI hard limit is 16 (confirmed via validate_bd_design
# probe), so the 17 masters split into two SmartConnects across HP0/HP1
# rather than one N-to-1, per the plan's contingency:
#   sc_a (9 SI, -> HP0): add(3) + conv(4) + gelu(2)
#   sc_b (8 SI, -> HP1): dwconv(4) + pwconv(4)
# Only one op_code's worker is ever active at a time (mutually exclusive
# dispatch), so this split is purely about balancing SmartConnect
# congestion/LUT, not about real concurrent bandwidth needs.
#
# 用法: vivado -mode batch -source run_impl_w8a4_tierb_200mhz.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_w8a4_tierb_200mhz_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_tierb "$script_dir/../fastvit_ip_w8a4/tier_b_rtl_ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_tierb]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (HP0 + HP1, 17 独立 master 分两组接) ────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_USE_S_AXI_HP1            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {200} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

# ── Tier B 顶层 (17 个独立 m_axi master, 手写 dispatch, HLS 黑盒 worker) ──
create_bd_cell -type ip -vlnv user.org:hls:fastvit_top_tierb:1.0 tierb_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_200M

# ── AXI-Lite 控制总线: PS GP0 → 2 Slaves (s_axi_control + s_axi_ctrl) ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 17 独立 master 分两组 SmartConnect → HP0/HP1 ──
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_a
set_property CONFIG.NUM_SI 9 [get_bd_cells sc_a]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_b
set_property CONFIG.NUM_SI 8 [get_bd_cells sc_b]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_200M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins tierb_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP1_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
foreach i {00 01} {
    connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M${i}_ACLK]
}
connect_bd_net $clk [get_bd_pins sc_a/aclk]
connect_bd_net $clk [get_bd_pins sc_b/aclk]

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
connect_bd_net $rstn [get_bd_pins sc_a/aresetn]
connect_bd_net $rstn [get_bd_pins sc_b/aresetn]

# ── AXI-Lite 控制连接 ─────────────────────────────────────
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins tierb_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI] [get_bd_intf_pins tierb_0/s_axi_ctrl]

# ── AXI 数据连接: group A (add+conv+gelu, 9) -> sc_a -> HP0 ──
set idx 0
foreach m {add_gmem0 add_gmem1 add_gmem3 \
           conv_gmem0 conv_gmem1 conv_gmem2 conv_gmem3 \
           gelu_gmem0 gelu_gmem3} {
    set si [format "S%02d_AXI" $idx]
    connect_bd_intf_net [get_bd_intf_pins tierb_0/m_axi_$m] [get_bd_intf_pins sc_a/$si]
    incr idx
}
connect_bd_intf_net [get_bd_intf_pins sc_a/M00_AXI] [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ── AXI 数据连接: group B (dwconv+pwconv, 8) -> sc_b -> HP1 ──
set idx 0
foreach m {dwconv_gmem0 dwconv_gmem1 dwconv_gmem2 dwconv_gmem3 \
           pwconv_gmem0 pwconv_gmem1 pwconv_gmem2 pwconv_gmem3} {
    set si [format "S%02d_AXI" $idx]
    connect_bd_intf_net [get_bd_intf_pins tierb_0/m_axi_$m] [get_bd_intf_pins sc_b/$si]
    incr idx
}
connect_bd_intf_net [get_bd_intf_pins sc_b/M00_AXI] [get_bd_intf_pins ps7_0/S_AXI_HP1]

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
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default              [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

# ── Utilization + Timing Report ──────────────────────────
open_run impl_1
set rpt_file "$proj_dir/utilization_w8a4_tierb_200mhz.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_w8a4_tierb_200mhz.rpt"
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
