# run_impl.tcl - FastVIT 5-IP Block Design + 综合 + 实现 + Bitstream
#
# AXI 拓扑 (v3: 双HP口, 串行IP生成避免rodin slave耗尽):
#   conv_ip(4) + dwconv_ip(4) + pwconv_ip(4) + add_ip(3) = 15路 → SC_main(15) → HP0
#   pool_ip(2)                                                    → SC_pool(2)  → HP1
#
# 修复: set_param general.maxThreads 1 串行生成SmartConnect IP
# 用法: vivado -mode batch -source run_impl.tcl -nolog -nojournal

# 关键: 串行IP生成，避免 "Could not create slave interpreter 'rodin'" 错误
set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_v3_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_conv   "$script_dir/../conv_ip/conv_ip_proj/solution1/impl/ip"
set ip_dw     "$script_dir/../dwconv_ip/dwconv_ip_proj/solution1/impl/ip"
set ip_pw     "$script_dir/../pwconv_ip/pwconv_ip_proj/solution1/impl/ip"
set ip_add    "$script_dir/../add_ip/add_ip_proj/solution1/impl/ip"
set ip_pool   "$script_dir/../pool_ip/pool_ip_proj/solution1/impl/ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list \
    [file normalize $ip_conv] \
    [file normalize $ip_dw]   \
    [file normalize $ip_pw]   \
    [file normalize $ip_add]  \
    [file normalize $ip_pool] \
] [current_project]
update_ip_catalog

# ── Block Design ──────────────────────────────────────────
create_bd_design "fastvit_bd"

# ── PS7 (启用 HP0 + HP1) ─────────────────────────────────
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

# ── HLS IP 核 ─────────────────────────────────────────────
create_bd_cell -type ip -vlnv user.org:hls:conv_ip:1.0    conv_ip_0
create_bd_cell -type ip -vlnv user.org:hls:dwconv_ip:1.0  dwconv_ip_0
create_bd_cell -type ip -vlnv user.org:hls:pwconv_ip:1.0  pwconv_ip_0
create_bd_cell -type ip -vlnv user.org:hls:pool_ip:1.0    pool_ip_0
create_bd_cell -type ip -vlnv user.org:hls:add_ip:1.0     add_ip_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

# ── AXI-Lite 控制总线: PS GP0 → 10 Slaves ─────────────────
# 每个HLS IP有两个AXI-Lite接口: s_axi_control(地址) + s_axi_ctrl(参数)
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI  1  [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 10  [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线 (v2: 两组, 减少SmartConnect层数) ─────────
# SC_main: conv_ip(4) + dwconv_ip(4) + pwconv_ip(4) + add_ip(3) = 15路 → HP0
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_main
set_property CONFIG.NUM_SI 15 [get_bd_cells sc_main]

# SC_pool: pool_ip(2) → HP1
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_pool
set_property CONFIG.NUM_SI 2 [get_bd_cells sc_pool]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins conv_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins dwconv_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins pwconv_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins pool_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins add_ip_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP1_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
foreach i {00 01 02 03 04 05 06 07 08 09} {
    connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M${i}_ACLK]
}
connect_bd_net $clk [get_bd_pins sc_main/aclk]
connect_bd_net $clk [get_bd_pins sc_pool/aclk]

# ── 复位连接 ──────────────────────────────────────────────
connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] \
               [get_bd_pins rst_ps7_0_100M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_100M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins conv_ip_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins dwconv_ip_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins pwconv_ip_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins pool_ip_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins add_ip_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
foreach i {00 01 02 03 04 05 06 07 08 09} {
    connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M${i}_ARESETN]
}
connect_bd_net $rstn [get_bd_pins sc_main/aresetn]
connect_bd_net $rstn [get_bd_pins sc_pool/aresetn]

# ── AXI-Lite 控制连接 ─────────────────────────────────────
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]       [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI]    [get_bd_intf_pins conv_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI]    [get_bd_intf_pins conv_ip_0/s_axi_ctrl]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M02_AXI]    [get_bd_intf_pins dwconv_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M03_AXI]    [get_bd_intf_pins dwconv_ip_0/s_axi_ctrl]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M04_AXI]    [get_bd_intf_pins pwconv_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M05_AXI]    [get_bd_intf_pins pwconv_ip_0/s_axi_ctrl]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M06_AXI]    [get_bd_intf_pins pool_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M07_AXI]    [get_bd_intf_pins pool_ip_0/s_axi_ctrl]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M08_AXI]    [get_bd_intf_pins add_ip_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M09_AXI]    [get_bd_intf_pins add_ip_0/s_axi_ctrl]

# ── AXI 数据连接 ─────────────────────────────────────────
# SC_main: conv_ip(4) + dwconv_ip(4) + pwconv_ip(4) + add_ip(3) → HP0
connect_bd_intf_net [get_bd_intf_pins conv_ip_0/m_axi_gmem0]   [get_bd_intf_pins sc_main/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins conv_ip_0/m_axi_gmem1]   [get_bd_intf_pins sc_main/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins conv_ip_0/m_axi_gmem2]   [get_bd_intf_pins sc_main/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins conv_ip_0/m_axi_gmem3]   [get_bd_intf_pins sc_main/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins dwconv_ip_0/m_axi_gmem0] [get_bd_intf_pins sc_main/S04_AXI]
connect_bd_intf_net [get_bd_intf_pins dwconv_ip_0/m_axi_gmem1] [get_bd_intf_pins sc_main/S05_AXI]
connect_bd_intf_net [get_bd_intf_pins dwconv_ip_0/m_axi_gmem2] [get_bd_intf_pins sc_main/S06_AXI]
connect_bd_intf_net [get_bd_intf_pins dwconv_ip_0/m_axi_gmem3] [get_bd_intf_pins sc_main/S07_AXI]
connect_bd_intf_net [get_bd_intf_pins pwconv_ip_0/m_axi_gmem0] [get_bd_intf_pins sc_main/S08_AXI]
connect_bd_intf_net [get_bd_intf_pins pwconv_ip_0/m_axi_gmem1] [get_bd_intf_pins sc_main/S09_AXI]
connect_bd_intf_net [get_bd_intf_pins pwconv_ip_0/m_axi_gmem2] [get_bd_intf_pins sc_main/S10_AXI]
connect_bd_intf_net [get_bd_intf_pins pwconv_ip_0/m_axi_gmem3] [get_bd_intf_pins sc_main/S11_AXI]
connect_bd_intf_net [get_bd_intf_pins add_ip_0/m_axi_gmem0]    [get_bd_intf_pins sc_main/S12_AXI]
connect_bd_intf_net [get_bd_intf_pins add_ip_0/m_axi_gmem1]    [get_bd_intf_pins sc_main/S13_AXI]
connect_bd_intf_net [get_bd_intf_pins add_ip_0/m_axi_gmem2]    [get_bd_intf_pins sc_main/S14_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_main/M00_AXI]         [get_bd_intf_pins ps7_0/S_AXI_HP0]

# SC_pool: pool_ip(2) → HP1
connect_bd_intf_net [get_bd_intf_pins pool_ip_0/m_axi_gmem0]   [get_bd_intf_pins sc_pool/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins pool_ip_0/m_axi_gmem1]   [get_bd_intf_pins sc_pool/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_pool/M00_AXI]         [get_bd_intf_pins ps7_0/S_AXI_HP1]

# ── 地址分配 ─────────────────────────────────────────────
assign_bd_address

# ── Validate & Save ───────────────────────────────────────
validate_bd_design
save_bd_design

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

# ── Implementation ────────────────────────────────────────
puts ">>> Launching Implementation (jobs=4)..."
launch_runs impl_1 -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

# ── Generate Bitstream ────────────────────────────────────
puts ">>> Generating Bitstream..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Bitstream done: [get_property STATUS [get_runs impl_1]]"

# ── Utilization Report ────────────────────────────────────
open_run impl_1
set rpt_file "$proj_dir/utilization_impl.rpt"
report_utilization -file $rpt_file
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (5-IP)"
puts "========================================"
report_utilization
puts ""
puts ">>> Bitstream: $proj_dir/${proj_name}.runs/impl_1/fastvit_bd_wrapper.bit"
puts ">>> Report:    $rpt_file"
