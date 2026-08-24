# run_impl_mac_array_a3_sol26_pwflat.tcl -- A3 run_layer rewrite stage 3
# (ZHR-92, 2026-08-24): first P&R of PW's flat-pipeline rewrite. DW
# untouched, so this isolates PW's own resource/timing delta against the
# solution22-era baseline (last confirmed: LUT ~57-59% real post-route,
# WNS +0.112ns at X0-96). csynth estimate for this round: 52,060 LUT
# (97%), only +654 LUT (+1.3%) over the pre-rewrite baseline (51,406,
# 96%) -- essentially flat at the csynth-estimate level, so X0-96 (the
# last-confirmed-working size at a near-identical estimate) is the
# starting pblock, not a default carry-forward: the decision criterion
# (LUT direction from the immediately preceding confirmed baseline) says
# "barely moved" here, which is what licenses reusing the last size,
# not just habit. Route-then-stop only, no phys_opt, no bitstream this
# round (ZHR-17 discipline) -- if this fails to route or WNS goes
# negative, STOP and report; do not chain into a pblock resize or a
# second attempt in the same round (explicit instruction, ZHR-92).
#
# 用法: vivado -mode batch -source run_impl_mac_array_a3_sol26_pwflat.tcl -nolog -nojournal
#
# Gotcha found this round: solution26's exported component.xml has
# vendor=xilinx.com, NOT user.org like solution22's did (some Vitis HLS
# project-level default differs, root cause not chased) -- first attempt
# using the sol22 template's VLNV verbatim (user.org:hls:...) failed
# with "IP definition not found" even though the repo loaded correctly.
# Always grep the actual component.xml's <spirit:vendor> before reusing
# a VLNV string across solutions.

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "a3wp23"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_mac_array "$script_dir/../fastvit_ip_v2/mac_array_poc_a3_axi/solution26/impl/ip"

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

# ── pblock (X0-96, "tight" variant) -- see header comment for why this
# is the re-evaluated starting choice, not a default carry-forward. ──
set_property STEPS.PLACE_DESIGN.TCL.PRE \
    [file normalize "$script_dir/pblock_mac_array_pre_place_tight.tcl"] [get_runs impl_1]

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
puts " POST-ROUTE UTILIZATION (mac_array_top, PW flat pipeline, DW untouched)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Report: $rpt_file"
puts ">>> Timing: $timing_rpt"
