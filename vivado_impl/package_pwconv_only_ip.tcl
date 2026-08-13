# package_pwconv_only_ip.tcl -- Package the pwconv-only Tier B minimal
# harness (single HLS black-box worker + its private per-bundle AXI4
# adapters, no other workers) as a reusable Vivado IP, for a standalone
# 200MHz P&R timing-closure test. Isolates whether pwconv_worker alone
# (unmodified from the combined Tier B design) can meet 5ns even with zero
# cross-worker sharing/muxing -- see typed-knitting-nygaard.md plan and
# gen_single_worker_tops.py / gen_package_tcl.py (session scratchpad).
#
# 用法: vivado -mode batch -source package_pwconv_only_ip.tcl -nolog -nojournal

set_param general.maxThreads 1

set script_dir [file normalize [file dirname [info script]]]
set rtl_dir    "$script_dir/../fastvit_ip_w8a4/tier_b_single"
set proj_name  "pwconv_only_pack_proj"
set proj_dir   "$script_dir/$proj_name"
set ip_dir     [file normalize "$script_dir/../fastvit_ip_w8a4/tier_b_single_ip/pwconv"]

create_project $proj_name $proj_dir -part xc7z020clg400-1 -force

set v_files [glob -nocomplain "$rtl_dir/*.v"]
add_files -norecurse $v_files
set_property top pwconv_only_top [current_fileset]
update_compile_order -fileset sources_1

file mkdir $ip_dir

ipx::package_project -root_dir $ip_dir -vendor user.org -library hls -taxonomy /UserIP \
    -import_files -set_current true -force

set core [ipx::current_core]
set_property name        pwconv_only_top   $core
set_property display_name "FastVIT pwconv Only (Tier B isolation test)" $core
set_property description "Standalone pwconv_worker + private AXI4 adapters, isolated from all other workers, for a minimal-harness 200MHz timing test" $core
set_property version "1.0" $core

ipx::merge_project_changes files $core

foreach bif [ipx::get_bus_interfaces -of_objects $core] {
    set bname [get_property NAME $bif]
    if {$bname eq "ap_clk" || $bname eq "ap_rst_n" || $bname eq "interrupt"} { continue }
    ipx::associate_bus_interfaces -busif $bname -clock ap_clk $core
}

# Same bus-parameter fix that solved the LUT-blowup bug in the combined
# Tier B packaging (READ_WRITE_MODE / HAS_BURST / outstanding-transaction
# counts) -- see package_tierb_ip.tcl for the full root-cause writeup.
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
puts " BUS INTERFACES DETECTED (pwconv_only_top)"
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
