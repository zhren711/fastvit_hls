# run_impl_v15.tcl — FastVIT unified single-IP v1.5 Block Design + synth + impl + bitstream
#
# v1.5 dedicated-bundle experiment (see fastvit_ip/fastvit_ip.h / .cpp header for the
# full root-cause writeup): the 200MHz attempt on v1.2 (4 shared m_axi masters gated
# by op_code) failed with WNS=-2.87ns, and diagnosis (report_timing top-10 paths, three
# different Vivado placement strategies) traced it to op_code fanning out into the
# auto-generated FIFO adapter logic of each of the 4 shared masters. v1.5 gives
# conv/dwconv/pwconv each their OWN 4 m_axi masters (12 total) instead of sharing;
# add/gelu still share 3 masters (lighter, less frequent ops). 15 masters total,
# vs 4 in v1.2 and 17 in the pre-merge 5-IP design.
#
# This script wires each op group's masters into its OWN SmartConnect feeding its OWN
# PS7 HP port (HP0=conv, HP1=dw, HP2=pw, HP3=shared) instead of one SmartConnect
# fanning everything into a single HP0 -- avoids recreating a single large arbitration
# point and lets each group's traffic move independently, matching the "let Vivado's
# proven SmartConnect do the sharing" plan in the v1.5 header comment.
#
# Clock is 100MHz (7ns) in THIS script -- deliberately a sanity check first (does the
# 15-master/4-HP-port topology place and route at all, what does LUT utilization look
# like, does WNS stay positive) before ever re-attempting 200MHz. See project notes:
# jumping straight to a new clock target on unverified RTL has bitten this project
# before (CONV_TN=4 hung the board despite clean HLS+Vivado numbers).
#
# 用法: vivado -mode batch -source run_impl_v15.tcl -nolog -nojournal

# 关键: 串行IP生成，避免 "Could not create slave interpreter 'rodin'" 错误
set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_unified_v15_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_fastvit "$script_dir/../fastvit_ip/fastvit_ip_proj/solution1/impl/ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_fastvit]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (4路 HP 口，每组 op 一个) ────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_USE_S_AXI_HP1            {1} \
    CONFIG.PCW_USE_S_AXI_HP2            {1} \
    CONFIG.PCW_USE_S_AXI_HP3            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

# ── 统一 HLS IP 核 v1.5 (conv/dwconv/pwconv 独立 bundle) ──
create_bd_cell -type ip -vlnv user.org:hls:fastvit_ip:1.5 fastvit_ip_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

# ── AXI-Lite 控制总线: PS GP0 → 2 Slaves (s_axi_control + s_axi_ctrl) ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 4 组独立 SmartConnect，各自接一个 HP 口 ──
#    conv: 4 masters → sc_conv → HP0
#    dw:   4 masters → sc_dw   → HP1
#    pw:   4 masters → sc_pw   → HP2
#    shared(add/gelu): 3 masters → sc_shared → HP3
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_conv
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_conv]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_dw
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_dw]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_pw
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_pw]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_shared
set_property CONFIG.NUM_SI 3 [get_bd_cells sc_shared]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins fastvit_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP1_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP2_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP3_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
foreach i {00 01} {
    connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M${i}_ACLK]
}
foreach sc {sc_conv sc_dw sc_pw sc_shared} {
    connect_bd_net $clk [get_bd_pins $sc/aclk]
}

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
foreach sc {sc_conv sc_dw sc_pw sc_shared} {
    connect_bd_net $rstn [get_bd_pins $sc/aresetn]
}

# ── AXI-Lite 控制连接 ─────────────────────────────────────
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins fastvit_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI] [get_bd_intf_pins fastvit_ip_0/s_axi_ctrl]

# ── AXI 数据连接: conv 组 (4 masters) → sc_conv → HP0 ──────
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_conv0] [get_bd_intf_pins sc_conv/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_conv1] [get_bd_intf_pins sc_conv/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_conv2] [get_bd_intf_pins sc_conv/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_conv3] [get_bd_intf_pins sc_conv/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_conv/M00_AXI]                [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ── AXI 数据连接: dw 组 (4 masters) → sc_dw → HP1 ──────────
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_dw0] [get_bd_intf_pins sc_dw/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_dw1] [get_bd_intf_pins sc_dw/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_dw2] [get_bd_intf_pins sc_dw/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_dw3] [get_bd_intf_pins sc_dw/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_dw/M00_AXI]               [get_bd_intf_pins ps7_0/S_AXI_HP1]

# ── AXI 数据连接: pw 组 (4 masters) → sc_pw → HP2 ──────────
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_pw0] [get_bd_intf_pins sc_pw/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_pw1] [get_bd_intf_pins sc_pw/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_pw2] [get_bd_intf_pins sc_pw/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_pw3] [get_bd_intf_pins sc_pw/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_pw/M00_AXI]               [get_bd_intf_pins ps7_0/S_AXI_HP2]

# ── AXI 数据连接: shared 组 (add/gelu, 3 masters) → sc_shared → HP3 ──
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_shared0] [get_bd_intf_pins sc_shared/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_shared1] [get_bd_intf_pins sc_shared/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins fastvit_ip_0/m_axi_gmem_shared2] [get_bd_intf_pins sc_shared/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_shared/M00_AXI]               [get_bd_intf_pins ps7_0/S_AXI_HP3]

# ── 地址分配 ─────────────────────────────────────────────
assign_bd_address

# ── Validate & Save ───────────────────────────────────────
validate_bd_design
save_bd_design

# ── 显式生成 BD IP output products (串行，避免 launch_runs 内部隐式并行生成
#    触发 "Could not create slave interpreter 'rodin'" 竞争错误) ─────────
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

# ── Implementation (同 v12/v13/v14/unified 的 directive，便于公平对比) ──
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
set rpt_file "$proj_dir/utilization_v15.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_v15.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (v1.5, 4 HP ports, 100MHz)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
