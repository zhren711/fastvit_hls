# run_impl_add_only_200mhz.tcl -- add-only Tier B minimal harness:
# Block Design + synth + impl at 200MHz, to test whether add_worker
# (unmodified HLS black box) alone, with fully private/unshared AXI4
# adapters (no cross-worker muxing at all), can meet 5ns in real P&R.
# Companion to package_add_only_ip.tcl (must run first). See
# typed-knitting-nygaard.md plan for context: HLS's own csynth estimate
# for add_worker already misses 5ns in isolation (pre-P&R); this is
# the authoritative post-P&R check.
#
# 用法: vivado -mode batch -source run_impl_add_only_200mhz.tcl -nolog -nojournal

set_param general.maxThreads 1

set part      "xc7z020clg400-1"
set proj_name "fastvit_add_only_200mhz_proj"
set script_dir [file normalize [file dirname [info script]]]
set proj_dir   "$script_dir/$proj_name"

set ip_dir "$script_dir/../fastvit_ip_w8a4/tier_b_single_ip/add"

create_project $proj_name $proj_dir -part $part -force
set_property ip_repo_paths [list [file normalize $ip_dir]] [current_project]
update_ip_catalog -rebuild

create_bd_design "fastvit_bd"

create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 ps7_0
set_property -dict [list \
    CONFIG.PCW_USE_S_AXI_HP0            {1} \
    CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {200} \
    CONFIG.PCW_EN_CLK0_PORT             {1} \
    CONFIG.PCW_USE_M_AXI_GP0            {1} \
] [get_bd_cells ps7_0]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins ps7_0/FIXED_IO]

create_bd_cell -type ip -vlnv user.org:hls:add_only_top:1.0 worker_0

create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps7_0_200M

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 ps_ctrl_ic
set_property CONFIG.NUM_SI 1 [get_bd_cells ps_ctrl_ic]
set_property CONFIG.NUM_MI 2 [get_bd_cells ps_ctrl_ic]

create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_main
set_property CONFIG.NUM_SI 3 [get_bd_cells sc_main]

set clk [get_bd_pins ps7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins rst_ps7_0_200M/slowest_sync_clk]
connect_bd_net $clk [get_bd_pins worker_0/ap_clk]
connect_bd_net $clk [get_bd_pins ps7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins ps7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/ACLK]
connect_bd_net $clk [get_bd_pins ps_ctrl_ic/S00_ACLK]
foreach i {00 01} {
    connect_bd_net $clk [get_bd_pins ps_ctrl_ic/M${i}_ACLK]
}
connect_bd_net $clk [get_bd_pins sc_main/aclk]

connect_bd_net [get_bd_pins ps7_0/FCLK_RESET0_N] \
               [get_bd_pins rst_ps7_0_200M/ext_reset_in]
set rstn [get_bd_pins rst_ps7_0_200M/peripheral_aresetn]
connect_bd_net $rstn [get_bd_pins worker_0/ap_rst_n]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/ARESETN]
connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/S00_ARESETN]
foreach i {00 01} {
    connect_bd_net $rstn [get_bd_pins ps_ctrl_ic/M${i}_ARESETN]
}
connect_bd_net $rstn [get_bd_pins sc_main/aresetn]

connect_bd_intf_net [get_bd_intf_pins ps7_0/M_AXI_GP0]    [get_bd_intf_pins ps_ctrl_ic/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M00_AXI] [get_bd_intf_pins worker_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins ps_ctrl_ic/M01_AXI] [get_bd_intf_pins worker_0/s_axi_ctrl]

set idx 0
foreach m {gmem0 gmem1 gmem3} {
    set si [format "S%02d_AXI" $idx]
    connect_bd_intf_net [get_bd_intf_pins worker_0/m_axi_add_$m] [get_bd_intf_pins sc_main/$si]
    incr idx
}
connect_bd_intf_net [get_bd_intf_pins sc_main/M00_AXI] [get_bd_intf_pins ps7_0/S_AXI_HP0]

assign_bd_address

validate_bd_design
save_bd_design

generate_target all [get_files fastvit_bd.bd]

make_wrapper -files [get_files fastvit_bd.bd] -top
set wrapper_files [get_files -filter {NAME =~ *fastvit_bd_wrapper.v}]
if {[llength $wrapper_files] == 0} {
    set wrapper "$proj_dir/${proj_name}.gen/sources_1/bd/fastvit_bd/hdl/fastvit_bd_wrapper.v"
    add_files -norecurse $wrapper
}
set_property top fastvit_bd_wrapper [current_fileset]
update_compile_order -fileset sources_1

puts ">>> Launching Synthesis (jobs=4)..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synthesis status: [get_property STATUS [get_runs synth_1]]"
if {[get_property PROGRESS [get_runs synth_1]] != "100%"} { error "Synthesis FAILED" }

set_property STEPS.OPT_DESIGN.ARGS.DIRECTIVE ExploreWithRemap       [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE ExtraTimingOpt       [get_runs impl_1]
# 2026-08-07: same placement-level fix validated on dwconv/pwconv (round
# 3 of the dwconv/pwconv timing investigation, typed-knitting-nygaard.md)
# -- post-route phys_opt_design sees actual routed delays, the standard
# lever once a path is route-delay-dominated rather than logic-depth-
# dominated. Applying here since add was never revisited after the
# original 2026-08-06 isolation experiment (-0.215ns).
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true                  [get_runs impl_1]
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]

puts ">>> Launching Implementation + Bitstream (jobs=4)..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1
puts "Implementation status: [get_property STATUS [get_runs impl_1]]"
if {[get_property PROGRESS [get_runs impl_1]] != "100%"} { error "Implementation FAILED" }

open_run impl_1
set rpt_file "$proj_dir/utilization_add_only_200mhz.rpt"
report_utilization -file $rpt_file

set timing_rpt "$proj_dir/timing_detail_add_only_200mhz.rpt"
report_timing -delay_type max -max_paths 10 -sort_by group -file $timing_rpt
puts ">>> Detailed timing report: $timing_rpt"
puts ""
puts "========================================"
puts " POST-IMPLEMENTATION UTILIZATION (add-only, 200MHz)"
puts "========================================"
report_utilization
set wns [get_property STATS.WNS [get_runs impl_1]]
set tns [get_property STATS.TNS [get_runs impl_1]]
puts ""
puts ">>> WNS: $wns ns"
puts ">>> TNS: $tns ns"
puts ">>> Report:    $rpt_file"
