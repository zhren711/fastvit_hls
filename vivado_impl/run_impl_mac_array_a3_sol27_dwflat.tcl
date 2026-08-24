# run_impl_mac_array_a3_sol27_dwflat.tcl -- A3 run_layer rewrite stage 4
# (ZHR-92, 2026-08-24): P&R for DW's flat pipeline (PW's is already
# board-verified at X0-120, see the sol26 round). Starting DIRECTLY at
# X0-120, not X0-96 -- the last REAL P&R success for this design FAMILY
# (flat-pipeline based) needed X0-120 (X0-96 failed there with 376
# unroutable pins, all tracing to gmem_meta's AXI read-FIFO output bus).
# solution27's own csynth LUT (51,135, 96.1%) is essentially unchanged
# from solution26's (52,060, 97%, slightly LOWER even) -- per CLAUDE.md's
# pblock re-evaluation rule, the relevant precedent to check against is
# the last REAL P&R state for this design shape, not blindly reverting
# to X0-96 (already falsified for a smaller, earlier version of this
# same flat-pipeline family).
#
# 用法: vivado -mode batch -source run_impl_mac_array_a3_sol27_dwflat.tcl -nolog -nojournal
#
# Gotcha carried over: solution27's exported component.xml vendor is
# xilinx.com, NOT user.org -- VLNV below already corrected (same as
# solution26's export).

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "a3wp25"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_mac_array "$script_dir/../fastvit_ip_v2/mac_array_poc_a3_axi/solution27/impl/ip"

# ── 新建工程 ──────────────────────────────────────────────
create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_mac_array]] [current_project]
update_ip_catalog -rebuild

# ── Block Design ──────────────────────────────────────────
create_bd_design "mac_array_bd"

# ── PS7 (1 HP port) ──────
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
create_bd_cell -type ip -vlnv xilinx.com:hls:mac_array_top:1.0 mac_array_top_0

# ── 复位控制器 ────────────────────────────────────────────
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

# ── AXI-Lite 控制总线 ──
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

# ── AXI-Lite 控制连接 ──
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
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED false [get_runs impl_1]

# ── pblock (X0-120, "wide" variant) -- see header comment for why this
# is the retry choice after X0-96's routing failure, matching the
# DW-burst precedent exactly. ──
set_property STEPS.PLACE_DESIGN.TCL.PRE \
    [file normalize "$script_dir/pblock_mac_array_pre_place_wide.tcl"] [get_runs impl_1]

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
puts " POST-ROUTE UTILIZATION (mac_array_top, PW + DW both flat pipelines)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Report: $rpt_file"
puts ">>> Timing: $timing_rpt"
