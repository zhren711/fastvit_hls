# package_tierb_ip.tcl -- Package the hand-written Tier B top level
# (fastvit_top_tierb.v + its pruned dependency set) as a reusable Vivado IP,
# for Phase B4 BD integration + 200MHz synth/impl attempt.
#
# Source RTL uses the exact same signal-naming convention as the HLS-generated
# fastvit_ip (ap_clk/ap_rst_n/s_axi_control_*/s_axi_ctrl_*/m_axi_<bundle>_*),
# so Vivado's standard AXI4/AXI4-Lite/clock/reset auto-inference (same as what
# Package IP's GUI wizard runs) should pick up all interfaces automatically:
#   - ap_clk            -> clock interface, ASSOCIATED_RESET=ap_rst_n
#   - ap_rst_n           -> reset interface
#   - s_axi_control_*    -> AXI4LITE slave
#   - s_axi_ctrl_*        -> AXI4LITE slave
#   - m_axi_<worker>_gmemN_* (17 of them) -> AXI4 master, one per bundle
#   - interrupt           -> plain external port (no bus interface)
#
# 用法: vivado -mode batch -source package_tierb_ip.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set rtl_dir    "$script_dir/../fastvit_ip_w8a4/tier_b_rtl"
set proj_name  "tierb_pack_proj"
set proj_dir   "$script_dir/$proj_name"
set ip_dir     [file normalize "$script_dir/../fastvit_ip_w8a4/tier_b_rtl_ip"]

create_project $proj_name $proj_dir -part xc7z020clg400-1 -force

# Add every .v file except the testbench (tb_fastvit_top_tierb.sv is not
# part of the IP; xelab/xsim verification already covered it separately).
set v_files [glob -nocomplain "$rtl_dir/*.v"]
add_files -norecurse $v_files
set_property top fastvit_top_tierb [current_fileset]
update_compile_order -fileset sources_1

file mkdir $ip_dir

ipx::package_project -root_dir $ip_dir -vendor user.org -library hls -taxonomy /UserIP \
    -import_files -set_current true -force

set core [ipx::current_core]
set_property name        fastvit_top_tierb   $core
set_property display_name "FastVIT Tier B Top" $core
set_property description "Hand-written Tier B top level: 4 HLS black-box workers + 1 hand-written GELU FSM, 17 independent AXI4 masters" $core
set_property version "1.0" $core

# Re-run interface auto-merge explicitly (in case the initial package_project
# pass didn't catch everything) and report what got inferred.
ipx::merge_project_changes files $core

# Associate every AXI (master + slave) bus interface with the ap_clk clock
# interface. Without this, ap_clk's ASSOCIATED_BUSIF stays empty and each
# AXI interface keeps whatever default FREQ_HZ package_project happened to
# infer (observed: a stale 100MHz default) -- which then hard-errors
# validate_bd_design with "FREQ_HZ does not match" the instant this IP is
# wired to a 200MHz clock net, since Vivado has no way to know these
# interfaces are supposed to inherit the connected clock's real frequency.
# The reference HLS-generated fastvit_ip IP already has this association
# (ASSOCIATED_BUSIF listing every m_axi/s_axi bundle) -- replicate it here.
foreach bif [ipx::get_bus_interfaces -of_objects $core] {
    set bname [get_property NAME $bif]
    if {$bname eq "ap_clk" || $bname eq "ap_rst_n" || $bname eq "interrupt"} { continue }
    ipx::associate_bus_interfaces -busif $bname -clock ap_clk $core
}

# Set NUM_READ_OUTSTANDING / NUM_WRITE_OUTSTANDING / MAX_BURST_LENGTH /
# SUPPORTS_NARROW_BURST on every m_axi master, matching exactly what the
# original HLS INTERFACE pragmas produced per-bundle in the reference
# fastvit_ip IP's component.xml (fastvit_ip_w8a4_proj/.../ip/component.xml):
#   gmem0 (feat_in, read-heavy):  READ=4,  WRITE=16
#   gmem1 (weight,  read-heavy):  READ=4,  WRITE=16
#   gmem2 (bias,    read-heavy):  READ=16, WRITE=16
#   gmem3 (feat_out,write-heavy): READ=16, WRITE=4
#   all bundles: MAX_BURST_LENGTH=256, SUPPORTS_NARROW_BURST=0
# Without these, ipx::package_project's plain AXI4 signal-name inference
# leaves them unset -- SmartConnect then has no per-port sizing hint and
# falls back to conservative (much larger) internal FIFO/reorder-buffer
# defaults on EVERY port, independent of topology. This was confirmed to be
# the real cause of the ~2-3x per-port LUT inflation seen across all
# SmartConnect split experiments (9+8, 16+1, 4-way) in this session -- none
# of which fixed it, because the actual lever was these missing bus
# parameters, not the topology.
array set outstanding_by_bundle {
    gmem0 {4  16}
    gmem1 {4  16}
    gmem2 {16 16}
    gmem3 {16 4}
}
# READ_WRITE_MODE + HAS_BURST: found via a live report_property diff between
# this project's working W8A4 100MHz baseline (fastvit_ip_0, cheap
# SmartConnect: sc_main NUM_SI=4 costs 2,633 LUT) and a same-NUM_SI=4 Tier B
# SmartConnect instance (7,678 LUT -- ~3x for the identical port count, so
# NUM_SI itself was already ruled out as the driver). The baseline declares
# every m_axi bundle's true directionality (gmem0/1/2 are READ_ONLY --
# feat_in/weight/bias are never written by the worker; gmem3 is WRITE_ONLY --
# feat_out is never read) and HAS_BURST=0 on all 4. Our packaged IP left both
# unset (generic AXI4 signal-name inference has no way to know a given
# bundle's physical AW/W/B (or AR) channel is present-but-never-driven),
# so SmartConnect defaulted to building full read+write crossbar logic on
# every one of the 17 ports instead of skipping the dead half -- the leading
# suspect for the LUT blowup once topology/outstanding-params/address-range
# were each ruled out by direct testing.
array set rw_mode_by_bundle {
    gmem0 READ_ONLY
    gmem1 READ_ONLY
    gmem2 READ_ONLY
    gmem3 WRITE_ONLY
}
foreach bif [ipx::get_bus_interfaces -of_objects $core] {
    set bname [get_property NAME $bif]
    if {![string match "m_axi_*" $bname]} { continue }
    set bundle [lindex [split $bname "_"] end]
    if {![info exists outstanding_by_bundle($bundle)]} {
        puts "WARNING: unrecognized bundle suffix for $bname, skipping outstanding-transaction params"
        continue
    }
    lassign $outstanding_by_bundle($bundle) rd wr
    foreach {pname pval} [list NUM_READ_OUTSTANDING $rd NUM_WRITE_OUTSTANDING $wr \
                                MAX_BURST_LENGTH 256 SUPPORTS_NARROW_BURST 0 \
                                HAS_BURST 0 READ_WRITE_MODE $rw_mode_by_bundle($bundle)] {
        ipx::add_bus_parameter $pname $bif
        set_property value $pval [ipx::get_bus_parameters $pname -of_objects $bif]
    }
}

puts ""
puts "======================================================================"
puts " BUS INTERFACES DETECTED"
puts "======================================================================"
foreach bif [ipx::get_bus_interfaces -of_objects $core] {
    puts [format "  %-28s %-20s %s" \
        [get_property NAME $bif] \
        [get_property INTERFACE_MODE $bif] \
        [get_property ABSTRACTION_TYPE_NAME $bif]]
}
puts "======================================================================"
puts " Total bus interfaces: [llength [ipx::get_bus_interfaces -of_objects $core]]"
puts " Total ports:          [llength [ipx::get_ports -of_objects $core]]"
puts "======================================================================"

ipx::create_xgui_files $core
ipx::update_checksums $core
ipx::save_core $core

puts ""
puts ">>> IP packaged at: $ip_dir"
puts ">>> component.xml:  [get_property XML_FILE_NAME $core]"
