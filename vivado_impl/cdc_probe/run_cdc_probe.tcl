# run_cdc_probe.tcl -- ZHR-92 (2026-09-17), (b) step 1: measure the per-transaction cost of an
# asynchronous clock crossing in SmartConnect / AXI Interconnect by direct XSim simulation, before
# deciding whether to build the clk_wiz + CDC BD for 111MHz.
#
#   vivado -mode batch -source run_cdc_probe.tcl -tclargs <name> <ic> <proto> <mclk_MHz> <sclk_MHz>
#     ic    = sc  (smartconnect, the data-path case: IP master -> HP0 slave)
#           = ic  (axi_interconnect 2.1, the control-path case: PS GP0 master -> s_axi_control)
#     proto = AXI4 | AXI4LITE
#   mclk == sclk -> single clock domain (the control); mclk != sclk -> async clock conversion.
#
# Master = AXI VIP (master mode), slave = axi_bram_ctrl + blk_mem_gen (real RTL, deterministic).
# The testbench (cdc_probe_tb.sv) issues single-beat and burst reads/writes and prints the
# blocking-call durations; the async-minus-sync difference is the CDC cost.
set name  [lindex $argv 0]
set ic    [lindex $argv 1]
set proto [lindex $argv 2]
set mclk  [lindex $argv 3]
set sclk  [lindex $argv 4]
set part  "xc7z020clg400-1"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir "$script_dir/proj_$name"

create_project cdc_$name $proj_dir -part $part -force
set bd "cdc_bd"
create_bd_design $bd

set mper [expr {1000.0 / $mclk}]
set sper [expr {1000.0 / $sclk}]
set async [expr {$mclk != $sclk}]
set slvclk [expr {$async ? "sclk" : "mclk"}]

create_bd_port -dir I -type clk -freq_hz [expr {int($mclk * 1000000)}] mclk
create_bd_port -dir I -type clk -freq_hz [expr {int($sclk * 1000000)}] sclk
create_bd_port -dir I -type rst aresetn
set_property CONFIG.POLARITY ACTIVE_LOW [get_bd_ports aresetn]

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_vip:1.1 axi_vip_0
set_property -dict [list CONFIG.PROTOCOL $proto CONFIG.INTERFACE_MODE {MASTER} \
    CONFIG.ADDR_WIDTH {32} CONFIG.DATA_WIDTH {32} CONFIG.ID_WIDTH {0} \
    CONFIG.HAS_BURST {1} CONFIG.HAS_LOCK {0} CONFIG.HAS_CACHE {0} CONFIG.HAS_REGION {0} \
    CONFIG.HAS_QOS {0} CONFIG.HAS_PROT {0} CONFIG.HAS_WSTRB {1} CONFIG.HAS_BRESP {1} CONFIG.HAS_RRESP {1}] \
    [get_bd_cells axi_vip_0]
if {$proto eq "AXI4LITE"} {
    set_property -dict [list CONFIG.HAS_BURST {0}] [get_bd_cells axi_vip_0]
}

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 axi_bram_ctrl_0
set_property -dict [list CONFIG.SINGLE_PORT_BRAM {1} CONFIG.DATA_WIDTH {32} CONFIG.PROTOCOL $proto] [get_bd_cells axi_bram_ctrl_0]
create_bd_cell -type ip -vlnv xilinx.com:ip:blk_mem_gen:8.4 blk_mem_gen_0
set_property -dict [list CONFIG.Memory_Type {Single_Port_RAM} CONFIG.use_bram_block {BRAM_Controller}] [get_bd_cells blk_mem_gen_0]
connect_bd_intf_net [get_bd_intf_pins axi_bram_ctrl_0/BRAM_PORTA] [get_bd_intf_pins blk_mem_gen_0/BRAM_PORTA]

if {$ic eq "sc"} {
    create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 icx
    set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1} CONFIG.NUM_CLKS [expr {$async ? 2 : 1}]] [get_bd_cells icx]
    connect_bd_net [get_bd_ports mclk] [get_bd_pins icx/aclk]
    if {$async} { connect_bd_net [get_bd_ports sclk] [get_bd_pins icx/aclk1] }
    connect_bd_net [get_bd_ports aresetn] [get_bd_pins icx/aresetn]
    connect_bd_intf_net [get_bd_intf_pins axi_vip_0/M_AXI] [get_bd_intf_pins icx/S00_AXI]
    connect_bd_intf_net [get_bd_intf_pins icx/M00_AXI] [get_bd_intf_pins axi_bram_ctrl_0/S_AXI]
} else {
    create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 icx
    set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] [get_bd_cells icx]
    connect_bd_net [get_bd_ports mclk] [get_bd_pins icx/ACLK]
    connect_bd_net [get_bd_ports mclk] [get_bd_pins icx/S00_ACLK]
    connect_bd_net [get_bd_ports $slvclk] [get_bd_pins icx/M00_ACLK]
    connect_bd_net [get_bd_ports aresetn] [get_bd_pins icx/ARESETN]
    connect_bd_net [get_bd_ports aresetn] [get_bd_pins icx/S00_ARESETN]
    connect_bd_net [get_bd_ports aresetn] [get_bd_pins icx/M00_ARESETN]
    connect_bd_intf_net [get_bd_intf_pins axi_vip_0/M_AXI] [get_bd_intf_pins icx/S00_AXI]
    connect_bd_intf_net [get_bd_intf_pins icx/M00_AXI] [get_bd_intf_pins axi_bram_ctrl_0/S_AXI]
}
connect_bd_net [get_bd_ports mclk] [get_bd_pins axi_vip_0/aclk]
connect_bd_net [get_bd_ports aresetn] [get_bd_pins axi_vip_0/aresetn]
connect_bd_net [get_bd_ports $slvclk] [get_bd_pins axi_bram_ctrl_0/s_axi_aclk]
connect_bd_net [get_bd_ports aresetn] [get_bd_pins axi_bram_ctrl_0/s_axi_aresetn]

assign_bd_address
set_property range 8K [get_bd_addr_segs {axi_vip_0/Master_AXI/SEG_axi_bram_ctrl_0_Mem0}]
validate_bd_design
save_bd_design
generate_target all [get_files $bd.bd]
make_wrapper -files [get_files $bd.bd] -top
add_files -norecurse [glob $proj_dir/cdc_$name.gen/sources_1/bd/$bd/hdl/${bd}_wrapper.v]

add_files -fileset sim_1 -norecurse "$script_dir/cdc_probe_tb.sv"
set_property top cdc_probe_tb [get_filesets sim_1]
set_property -name {xsim.simulate.runtime} -value {200us} -objects [get_filesets sim_1]
set_property verilog_define [list "CDC_PROTO_$proto"] [get_filesets sim_1]
set_property -name {xsim.simulate.xsim.more_options} -value "-testplusarg MPER=$mper -testplusarg SPER=$sper" -objects [get_filesets sim_1]
launch_simulation
# (launch_simulation already runs xsim.simulate.runtime)
set log [glob -nocomplain $proj_dir/cdc_$name.sim/sim_1/behav/xsim/simulate.log]; # results also echo to stdout
puts ">>> CDC_PROBE $name ic=$ic proto=$proto mclk=$mclk sclk=$sclk"
foreach l [split [read [open [lindex $log 0]]] "\n"] { if {[string match "*CDC_RESULT*" $l]} { puts $l } }
exit
