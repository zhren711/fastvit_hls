# run_impl_mac_array_a3.tcl -- A3 first P&R (ZHR-92, 2026-08-21): new
# architecture's mac_array_top (32-wide, 4x4x2), real AXI interface,
# 100MHz. First-ever P&R run of this architecture -- every prior number
# was a csynth estimate. Stops at route_design -- NO bitstream, NO
# phys_opt_design this round (ZHR-17: phys_opt_design, especially
# post-route, gets silently killed under foreground/heavy execution --
# splitting into a route-then-reopen-for-phys_opt two-phase recipe is
# the proven fix, but this round's question is just "does it route and
# what's WNS", which route_design alone answers).
#
# 4 masters (gmem_act shared in/out, gmem_w, gmem_b, gmem_meta) -> ONE
# SmartConnect -> ONE Zynq HP port. Old architecture's v18 script split
# masters across 2 HP ports specifically to route around op_code's 5-way
# FIFO-adapter decode reaching every group -- that reason doesn't exist
# here (no op_code, no shared dispatch register), so this stays simple
# (1 port) until real congestion data says otherwise, not preemptively
# copying a fix for a problem this architecture doesn't have.
#
# 用法: vivado -mode batch -source run_impl_mac_array_a3.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "mac_array_a3_proj_pblock"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_mac_array "$script_dir/../fastvit_ip_v2/mac_array_poc_a3_axi/solution4/impl/ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_mac_array]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "mac_array_bd"

# ── PS7 (1 HP port -- see header note on why not 2) ──────
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

# ── mac_array_top IP ──────────────────────────────────────
create_bd_cell -type ip -vlnv user.org:hls:mac_array_top:1.0 mac_array_top_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

# ── AXI-Lite 控制总线: PS GP0(AXI3) → axi_interconnect(协议转换) →
#    s_axi_control(AXI4LITE) -- 直连会因协议不匹配报错(AXI3 vs
#    AXI4LITE),v18 模板用同样的 axi_interconnect 做转换，这里复用 ──
create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 1 [get_bd_cells ps_ctrl_ic]

# ── AXI 数据总线: 4 masters → 1 SmartConnect → HP0 ───────
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_data
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_data]

# ── 时钟连接 ──────────────────────────────────────────────
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins mac_array_top_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins sc_data/aclk]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M00_ACLK]

# ── 复位连接 ──────────────────────────────────────────────
connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] \
               [get_bd_pins rst_ps7_0_100M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_100M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins mac_array_top_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins sc_data/aresetn]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M00_ARESETN]

# ── AXI-Lite 控制连接: GP0 → ps_ctrl_ic → s_axi_control ──
connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins mac_array_top_0/s_axi_control]

# ── AXI 数据连接: 4 masters → sc_data → HP0 ──────────────
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_act]  [get_bd_intf_pins sc_data/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_w]    [get_bd_intf_pins sc_data/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_b]    [get_bd_intf_pins sc_data/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_meta] [get_bd_intf_pins sc_data/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_data/M00_AXI]                 [get_bd_intf_pins ps7_0/S_AXI_HP0]

# ── 地址分配 ─────────────────────────────────────────────
assign_bd_address

# ── Validate & Save ───────────────────────────────────────
validate_bd_design
save_bd_design

# ── 显式生成 BD IP output products ────────────────────────
generate_target all [get_files mac_array_bd.bd]

# ── HDL Wrapper ───────────────────────────────────────────
make_wrapper -files [get_files mac_array_bd.bd] -top
set wrapper_files [get_files -filter {NAME =~ *mac_array_bd_wrapper.v}]
if {[llength $wrapper_files] == 0} {
    set wrapper "$proj_dir/${proj_name}.gen/sources_1/bd/mac_array_bd/hdl/mac_array_bd_wrapper.v"
    add_files -norecurse $wrapper
}
set_property top mac_array_bd_wrapper [current_fileset]
update_compile_order -fileset sources_1

# ── Synthesis ─────────────────────────────────────────────
puts ">>> Launching Synthesis (jobs=4)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synthesis status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

# ── Implementation: opt + place + route ONLY, no phys_opt, no bitstream
# (this round's question: does it route, what's WNS -- ZHR-17 discipline
# says don't chain phys_opt_design/write_bitstream into the same
# foreground-adjacent batch as a first attempt). ──────────
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED false [get_runs impl_1]

# ── Option E (ZHR-92): pblock constraining mac_array_top_0's whole
# instance to a compact physical region, targeting gmem_meta's
# placement-distance signature (zero logic levels + 74% routing delay,
# NOT high fan-out -- option D's diagnosis was wrong). Runs automatically
# before place_design via the managed run's TCL.PRE hook. ──────────
set_property STEPS.PLACE_DESIGN.TCL.PRE \
    [file normalize "$script_dir/pblock_mac_array_pre_place.tcl"] [get_runs impl_1]

puts ">>> Launching Implementation (opt+place+route only, jobs=4)..."
launch_runs impl_1 -to_step route_design -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"

# ── Utilization + Timing Report (whatever the outcome) ────
open_run impl_1
set rpt_file "$proj_dir/utilization_mac_array_a3.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_mac_array_a3.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-ROUTE UTILIZATION (mac_array_top, 32-wide, real AXI, 100MHz, A3 first P&R)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Report: $rpt_file"
puts ">>> Timing: $timing_rpt"
