# investigate_floorplan.tcl -- probe the existing routed dwconv_only /
# pwconv_only checkpoints to understand current placement spread, plus
# query the xc7z020clg400 device's own resource-column layout and the
# PS7 hard block's fixed location, before designing a Pblock constraint.
#
# 用法: vivado -mode batch -source investigate_floorplan.tcl -nolog -nojournal

set_param general.maxThreads 1
set script_dir [file normalize [file dirname [info script]]]

proc probe_worker {name proj_name} {
    puts ""
    puts "========================================================"
    puts " $name"
    puts "========================================================"
    set dcp [file normalize "$::script_dir/${proj_name}/${proj_name}.runs/impl_1/fastvit_bd_wrapper_routed.dcp"]
    if {![file exists $dcp]} {
        puts "DCP not found: $dcp"
        return
    }
    open_checkpoint $dcp

    puts "--- Device / part ---"
    puts [get_property PART [current_design]]

    puts "--- PS7 location (fixed hard block) ---"
    set ps7_cells [get_cells -hierarchical -filter {REF_NAME == "PS7"}]
    foreach c $ps7_cells {
        puts "  $c  LOC=[get_property LOC $c]"
    }

    puts "--- Worst path endpoints: LOC + clock region ---"
    set paths [get_timing_paths -delay_type max -max_paths 3 -nworst 1]
    foreach p $paths {
        set src [get_property STARTPOINT_PIN $p]
        set dst [get_property ENDPOINT_PIN $p]
        set src_cell [get_cells -of_objects [get_pins $src]]
        set dst_cell [get_cells -of_objects [get_pins $dst]]
        puts "  SRC: $src_cell  LOC=[get_property LOC $src_cell]  BEL=[get_property BEL $src_cell]"
        puts "  DST: $dst_cell  LOC=[get_property LOC $dst_cell]  BEL=[get_property BEL $dst_cell]"
        puts "  SLACK: [get_property SLACK $p]"
    }

    puts "--- Overall placed-cell bounding box for the whole worker hierarchy ---"
    set all_cells [get_cells -hierarchical -filter {IS_PRIMITIVE && LOC != ""}]
    set minx 999999; set maxx -1; set miny 999999; set maxy -1
    set n 0
    foreach c $all_cells {
        set loc [get_property LOC $c]
        if {[regexp {SLICE_X([0-9]+)Y([0-9]+)} $loc -> x y]} {
            if {$x < $minx} {set minx $x}
            if {$x > $maxx} {set maxx $x}
            if {$y < $miny} {set miny $y}
            if {$y > $maxy} {set maxy $y}
            incr n
        }
    }
    puts "  SLICE cells counted: $n"
    puts "  X range: $minx - $maxx"
    puts "  Y range: $miny - $maxy"

    puts "--- Device resource column ranges (report once, same for both) ---"
    set slice_x_max 0
    set slice_y_max 0
    foreach s [get_sites -filter {SITE_TYPE == SLICEL || SITE_TYPE == SLICEM}] {
        if {[regexp {SLICE_X([0-9]+)Y([0-9]+)} $s -> x y]} {
            if {$x > $slice_x_max} {set slice_x_max $x}
            if {$y > $slice_y_max} {set slice_y_max $y}
        }
    }
    puts "  Max SLICE_X: $slice_x_max   Max SLICE_Y: $slice_y_max"
    set ramb_x {}
    foreach s [get_sites -filter {SITE_TYPE == RAMB36}] {
        if {[regexp {RAMB36_X([0-9]+)Y} $s -> x]} { lappend ramb_x $x }
    }
    puts "  RAMB36 X columns: [lsort -unique -integer $ramb_x]"
    set dsp_x {}
    foreach s [get_sites -filter {SITE_TYPE == DSP48E1}] {
        if {[regexp {DSP48_X([0-9]+)Y} $s -> x]} { lappend dsp_x $x }
    }
    puts "  DSP48E1 X columns: [lsort -unique -integer $dsp_x]"

    close_design
}

probe_worker "DWCONV_ONLY" "fastvit_dwconv_only_200mhz_proj"
probe_worker "PWCONV_ONLY" "fastvit_pwconv_only_200mhz_proj"
