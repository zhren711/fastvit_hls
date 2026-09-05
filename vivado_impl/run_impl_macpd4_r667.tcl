# run_impl_macpd4_r667.tcl -- ZHR-92 150MHz re-check, point 1/3 (9.0ns/150MHz).
# Real P&R for the CURRENT architecture (MAC_PD=4, gmem_meta eliminated,
# WNS+0.339ns at 10ns/100MHz) with BOTH the HLS csynth (create_clock 9.0ns,
# see run_export_ip_macpd4_r667.tcl) AND the Vivado/PS7 clock (150MHz)
# tightened together -- the original 150/125/150MHz sweep never re-ran HLS
# csynth at the tighter period, only tightened the Vivado-side constraint on
# RTL scheduled for 10ns. Reports top-10 critical paths (not just WNS) and
# absolute data-path delay (not just slack, which is trivially more negative
# under a tighter constraint regardless of whether HLS rescheduling helped).
#
# route_design only, no phys_opt, no bitstream -- this is a timing-recon
# round per this project's own one-shot-per-point discipline, not a
# deliverable build.
#
# 用法: vivado -mode batch -source run_impl_macpd4_r667.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "macpd4r667imp"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_mac_array "$script_dir/../fastvit_ip_v2/macpd4_r667/s1/impl/ip"

create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_mac_array]] [current_project]
update_ip_catalog -rebuild

create_bd_design "mac_array_bd"

create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {150} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

create_bd_cell -type ip -vlnv xilinx.com:hls:mac_array_top:1.0 mac_array_top_0

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_150M

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 1 [get_bd_cells ps_ctrl_ic]

create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_data
set_property CONFIG.NUM_SI 3 [get_bd_cells sc_data]

set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_150M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins mac_array_top_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins sc_data/aclk]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M00_ACLK]

connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] \
               [get_bd_pins rst_ps7_0_150M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_150M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins mac_array_top_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins sc_data/aresetn]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M00_ARESETN]

connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins mac_array_top_0/s_axi_control]

connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_act]  [get_bd_intf_pins sc_data/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_w]    [get_bd_intf_pins sc_data/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_b]    [get_bd_intf_pins sc_data/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_data/M00_AXI]                 [get_bd_intf_pins ps7_0/S_AXI_HP0]

assign_bd_address

validate_bd_design
save_bd_design

generate_target all [get_files mac_array_bd.bd]

make_wrapper -files [get_files mac_array_bd.bd] -top
set wrapper_files [get_files -filter {NAME =~ *mac_array_bd_wrapper.v}]
if {[llength $wrapper_files] == 0} {
    set wrapper "$proj_dir/${proj_name}.gen/sources_1/bd/mac_array_bd/hdl/mac_array_bd_wrapper.v"
    add_files -norecurse $wrapper
}
set_property top mac_array_bd_wrapper [current_fileset]
update_compile_order -fileset sources_1

puts ">>> Launching Synthesis (jobs=4)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synthesis status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED false [get_runs impl_1]

puts ">>> Launching Implementation (opt+place+route only, jobs=4)..."
launch_runs impl_1 -to_step route_design -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"

open_run impl_1
set rpt_file "$proj_dir/utilization_macpd4_r667.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_macpd4_r667.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-ROUTE UTILIZATION (mac_array_top, MAC_PD=4, 9.0ns/150MHz)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Report: $rpt_file"
puts ">>> Timing: $timing_rpt"
