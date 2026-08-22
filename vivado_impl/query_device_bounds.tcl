link_design -part xc7z020clg400-1
set slice_xs {}
set slice_ys {}
foreach s [get_sites -filter {SITE_TYPE == SLICEL || SITE_TYPE == SLICEM}] {
    if {[regexp {SLICE_X([0-9]+)Y([0-9]+)} $s -> xcol yrow]} {
        lappend slice_xs $xcol
        lappend slice_ys $yrow
    }
}
set slice_xs [lsort -integer -unique $slice_xs]
set slice_ys [lsort -integer -unique $slice_ys]
puts ">>> SLICE X range: [lindex $slice_xs 0] to [lindex $slice_xs end]"
puts ">>> SLICE Y range: [lindex $slice_ys 0] to [lindex $slice_ys end]"

foreach {label pattern} {RAMB18 RAMB18_X* RAMB36 RAMB36_X* DSP48 DSP48_X*} {
    set xs {}
    set ys {}
    foreach s [get_sites -filter "NAME =~ \"$pattern\""] {
        if {[regexp {X([0-9]+)Y([0-9]+)} $s -> xcol yrow]} {
            lappend xs $xcol
            lappend ys $yrow
        }
    }
    set xs [lsort -integer -unique $xs]
    set ys [lsort -integer -unique $ys]
    puts ">>> $label X range: [lindex $xs 0] to [lindex $xs end], Y range: [lindex $ys 0] to [lindex $ys end]"
}
exit
