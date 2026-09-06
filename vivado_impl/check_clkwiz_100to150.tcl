# check_clkwiz_100to150.tcl -- ZHR-92 150MHz deployment-path investigation,
# item 1: is a clk_wiz (MMCM/PLL) 100MHz->150MHz generation feasible on
# xc7z020-1 at all (VCO range, M/D/O divider legality)? Real Vivado IP
# customization + validation, not hand arithmetic -- mirrors the PS7
# achievable-frequency check's own discipline (query the tool, don't guess).
create_project clkwiz_check ./clkwiz_check -part xc7z020clg400-1 -force

create_ip -name clk_wiz -vendor xilinx.com -library ip -version 6.0 -module_name clk_wiz_0
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {100.000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {150.000} \
    CONFIG.USE_LOCKED {true} \
    CONFIG.USE_RESET {true} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
    CONFIG.CLK_OUT1_PORT {clk_out150} \
] [get_ips clk_wiz_0]

generate_target {instantiation_template} [get_files clk_wiz_0.xci]

puts ">>> Reading back actual achieved clk_wiz configuration:"
foreach p [list_property [get_ips clk_wiz_0]] {
    if {[string match "*CLKOUT1_ACTUAL*" $p] || \
        [string match "*MMCM_CLKFBOUT_MULT*" $p] || \
        [string match "*MMCM_CLKIN1_PERIOD*" $p] || \
        [string match "*MMCM_DIVCLK_DIVIDE*" $p] || \
        [string match "*MMCM_CLKOUT0_DIVIDE*" $p] || \
        [string match "*PRIMITIVE*" $p] || \
        [string match "*JITTER*" $p]} {
        puts "    $p = [get_property $p [get_ips clk_wiz_0]]"
    }
}
puts ">>> done."
exit
