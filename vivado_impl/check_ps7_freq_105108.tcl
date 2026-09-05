# check_ps7_freq_105108.tcl -- ZHR-92 razor-thin-margin round: check what
# real achievable FCLK0 frequencies exist near 100/105/108MHz. Reads the
# PS7 IP's own computed "ACT" (actual, as opposed to requested) frequency
# property directly off the BD cell after validate_bd_design, rather than
# parsing a generated .xci file (the file-parsing approach in this
# project's own check_ps7_freq3.tcl assumed a fixed file location/name that
# didn't resolve this time -- querying the cell property directly is more
# robust and doesn't need generate_target at all).
create_project ps7freq_check4 ./ps7freq_check4 -part xc7z020clg400-1 -force
create_bd_design "freqtest"
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_100M
set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_100M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] [get_bd_pins rst_ps7_0_100M/ext_reset_in]

set all_props [list_property [get_bd_cells ps7_0]]
set act_props {}
foreach p $all_props {
    if {[string match "*ACT*FCLK0*" $p] || [string match "*ACT*FPGA0*" $p]} {
        lappend act_props $p
    }
}
puts ">>> matching ACT properties found: $act_props"

foreach target {100 105 108 109 110 111 112} {
    set_property -dict [list CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ $target] [get_bd_cells ps7_0]
    validate_bd_design -quiet
    puts ">>> requested=${target}MHz"
    foreach p $act_props {
        puts "    $p = [get_property $p [get_bd_cells ps7_0]]"
    }
}
puts ">>> done."
exit
