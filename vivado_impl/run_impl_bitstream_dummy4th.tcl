# run_impl_bitstream_dummy4th.tcl -- ZHR-92 (2026-09-05): DIAGNOSTIC ONLY,
# NOT A DEPLOYABLE CANDIDATE. Real P&R + bitstream for the DUMMY_FOURTH_
# MASTER isolation experiment: restores sc_data's NUM_SI to 4 (matching the
# pre-gmem_meta-elimination topology) via a 4th m_axi port (gmem_dummy)
# that is genuinely live in the RTL (dummy_enable is a real s_axilite
# runtime input, not a compile-time constant) but never actually issues a
# transaction in real dispatch (the ARM driver never sets dummy_enable).
# Isolates "does 4-way SmartConnect arbitration matter" from "does
# gmem_meta's own specific traffic matter" for the GELU/ADD regression
# bisected to the gmem_meta-elimination round. Same clock (10ns/100MHz),
# same 3 real masters (gmem_act/gmem_w/gmem_b) wired identically to the
# current macpd4 baseline -- ONLY sc_data's NUM_SI and the addition of
# gmem_dummy on S03_AXI differ. No pblock (none used on macpd4 either).

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "dummy4thimp"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_mac_array "$script_dir/../fastvit_ip_v2/dummy4th/s1/impl/ip"

create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_mac_array]] [current_project]
update_ip_catalog -rebuild

create_bd_design "mac_array_bd"

create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

create_bd_cell -type ip -vlnv xilinx.com:hls:mac_array_top:1.0 mac_array_top_0

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 1 [get_bd_cells ps_ctrl_ic]

create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_data
set_property CONFIG.NUM_SI 4 [get_bd_cells sc_data]

set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins mac_array_top_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins sc_data/aclk]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M00_ACLK]

connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] \
               [get_bd_pins rst_ps7_0_100M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_100M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins mac_array_top_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins sc_data/aresetn]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M00_ARESETN]

connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins mac_array_top_0/s_axi_control]

connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_act]   [get_bd_intf_pins sc_data/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_w]     [get_bd_intf_pins sc_data/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_b]     [get_bd_intf_pins sc_data/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins mac_array_top_0/m_axi_gmem_dummy] [get_bd_intf_pins sc_data/S03_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_data/M00_AXI]                  [get_bd_intf_pins ps7_0/S_AXI_HP0]

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

puts ">>> Launching Implementation (opt+place+route+bitstream, jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"

open_run impl_1
set rpt_file "$proj_dir/utilization_dummy4th.rpt"
report_utilization -file $rpt_file
set wns [get_property STATS.WNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> Bitstream: $proj_dir/$proj_name.runs/impl_1/mac_array_bd_wrapper.bit"
