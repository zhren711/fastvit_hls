# run_impl_v18.tcl — FastVIT unified single-IP v1.8 (grouped-master
# experiment) Block Design + synth + impl + bitstream, 100MHz FIRST.
#
# Middle ground between v1.2 (4 shared masters, all 5 ops, op_code needs
# a 5-way decode reaching every FIFO adapter -- blocks 200MHz, WNS
# -2.873ns, 7 independent fix attempts all failed) and v1.5 (15 fully
# independent masters -- eliminates sharing entirely but blows the LUT/
# control-set budget even at 100MHz: 2007 control sets, 119.6% LUT,
# placement failed).
#
# v1.8: conv/dwconv/pwconv (3 heavy, wide-datapath ops) keep sharing
# gmem0-3 (3-way decode, down from 5-way); add/gelu (2 cheap, narrow,
# frequent elementwise ops) move to their own gmem4-6 group (2-way
# decode). 7 masters total, wired to 2 SEPARATE SmartConnects on 2
# SEPARATE PS7 HP ports so the two groups can land in physically
# different regions of the die instead of one op_code bit reaching all
# 5 ops' worth of FIFO logic.
#
# 100MHz FIRST (not 200MHz): does this topology even place, what does
# LUT/control-set utilization look like vs the v1.2 baseline (56.34%
# LUT, WNS +0.163ns) and the failed v1.5 numbers (119.6% LUT)? Per this
# project's established practice (v1.5 was also sanity-checked at
# 100MHz first, which is exactly what caught its placement failure
# before ever wasting time on 200MHz).
#
# 用法: vivado -mode batch -source run_impl_v18.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_v18_200mhz_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_fastvit "$script_dir/../fastvit_ip/fastvit_ip_proj_v18_200mhz/solution1/impl/ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_fastvit]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (2路 HP 口: HP0=heavy组 conv/dw/pw, HP1=light组 add/gelu) ──
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

# ── 统一 HLS IP 核 v1.8 (grouped masters: gmem0-3 heavy, gmem4-6 light) ──
create_bd_cell -type ip -vlnv user.org:hls:fastvit_ip:1.9 fastvit_ip_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

# ── AXI-Lite 控制总线: PS GP0 → 2 Slaves (s_axi_control + s_axi_ctrl) ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 2 组 SmartConnect ──────────────────────
#    heavy (conv/dw/pw): gmem0-3 (4 masters) → sc_heavy → HP0
#    light (add/gelu):   gmem4-6 (3 masters) → sc_light → HP1
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_heavy
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_heavy]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_light
set_property CONFIG.NUM_SI 3 [get_bd_cells sc_light]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins fastvit_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP1_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
foreach i {00 01} {
    connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M${i}_ACLK]
}
connect_bd_net $clk [get_bd_pins sc_heavy/aclk]
connect_bd_net $clk [get_bd_pins sc_light/aclk]

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
connect_bd_net $rstn [get_bd_pins sc_heavy/aresetn]
connect_bd_net $rstn [get_bd_pins sc_light/aresetn]

# ── AXI-Lite 控制连接 ─────────────────────────────────────
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins fastvit_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI] [get_bd_intf_pins fastvit_ip_0/s_axi_ctrl]

# ── AXI 数据连接: heavy 组 (gmem0-3) → sc_heavy → HP0 ──────
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem0] [get_bd_intf_pins sc_heavy/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem1] [get_bd_intf_pins sc_heavy/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem2] [get_bd_intf_pins sc_heavy/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem3] [get_bd_intf_pins sc_heavy/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_heavy/M00_AXI]         [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ── AXI 数据连接: light 组 (gmem4-6, add/gelu) → sc_light → HP1 ──
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem4] [get_bd_intf_pins sc_light/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem5] [get_bd_intf_pins sc_light/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem6] [get_bd_intf_pins sc_light/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_light/M00_AXI]         [get_bd_intf_pins ps7_0/S_AXI_HP1]

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

# ── Implementation (同 v12/v13/v14/v15/unified 的 directive，便于公平对比) ──
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
set rpt_file "$proj_dir/utilization_v18_200mhz.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_v18_200mhz.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (v1.9, grouped masters, 2 HP ports, 200MHz)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns  (v1.2 baseline: +0.163ns @56.34% LUT; v1.5 full-independence FAILED to place @119.6% LUT)"
puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
