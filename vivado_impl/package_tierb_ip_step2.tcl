# package_tierb_ip_step2.tcl -- Package the Step 2 (gmem0/gmem3-private,
# gmem1/gmem2-shared) Tier B hand-written top level as a reusable Vivado IP.
#
# Adapted from package_tierb_ip.tcl (the 17-master / 8-master lineage) --
# same signal-naming convention (ap_clk/ap_rst_n/s_axi_control_*/
# s_axi_ctrl_*/m_axi_<bundle>_*), same auto-inference approach, same
# proven-necessary bus-parameter fixes (NUM_READ/WRITE_OUTSTANDING,
# READ_WRITE_MODE, HAS_BURST -- without these, SmartConnect defaults to
# a much larger per-port footprint, the root cause of the LUT blowup
# found and fixed earlier in this project's Tier B history). Only the
# source/output paths and bundle set differ: 12 masters now (conv/
# dwconv/pwconv/add/gelu x {gmem0,gmem3} private = 10, shared_gmem1 +
# shared_gmem2 = 2), vs the 8-master lineage's 4 private + 4 shared.
#
# 用法: vivado -mode batch -source package_tierb_ip_step2.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set rtl_dir    "$script_dir/../fastvit_ip_w8a4/tier_b_rtl_step2"
set proj_name  "tierb_pack_proj_step2"
set proj_dir   "$script_dir/$proj_name"
set ip_dir     [file normalize "$script_dir/../fastvit_ip_w8a4/tier_b_rtl_ip_step2"]

create_project $proj_name $proj_dir -part xc7z020clg400-1 -force

set v_files [glob -nocomplain "$rtl_dir/*.v"]
add_files -norecurse $v_files
set_property top fastvit_top_tierb [current_fileset]
update_compile_order -fileset sources_1

file mkdir $ip_dir

ipx::package_project -root_dir $ip_dir -vendor user.org -library hls -taxonomy /UserIP \
    -import_files -set_current true -force

set core [ipx::current_core]
set_property name        fastvit_top_tierb   $core
set_property display_name "FastVIT Tier B Top (Step 2: gmem0/gmem3-private)" $core
set_property description "Hand-written Tier B top level, Step 2 partial independence: 4 HLS black-box workers + 1 hand-written GELU FSM, gmem0(feat_in)/gmem3(feat_out) private per-worker (10 masters), gmem1(weight)/gmem2(bias) shared (2 masters), 12 total" $core
set_property version "1.0" $core

ipx::merge_project_changes files $core

foreach bif [ipx::get_bus_interfaces -of_objects $core] {
    set bname [get_property NAME $bif]
    if {$bname eq "ap_clk" || $bname eq "ap_rst_n" || $bname eq "interrupt"} { continue }
    ipx::associate_bus_interfaces -busif $bname -clock ap_clk $core
}

array set outstanding_by_bundle {
    gmem0 {4  16}
    gmem1 {4  16}
    gmem2 {16 16}
    gmem3 {16 4}
}
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
