# query_device_layout.tcl -- get full X/Y ranges for all relevant site
# types on xc7z020clg400-1, to build a correctly-bounded Pblock area
# spec covering SLICE + DSP48E1 + RAMB18E1 + RAMB36E1 together.
set_param general.maxThreads 1
link_design -part xc7z020clg400-1

proc range_of {filt} {
    set xs {}; set ys {}
    foreach s [get_sites -filter $filt] {
        if {[regexp {_X([0-9]+)Y([0-9]+)} $s -> x y]} {
            lappend xs $x; lappend ys $y
        }
    }
    if {[llength $xs] == 0} { return "none" }
    set xs [lsort -integer -unique $xs]
    set ys [lsort -integer -unique $ys]
    return "X: [lindex $xs 0]-[lindex $xs end]  Y: [lindex $ys 0]-[lindex $ys end]  (Xcols: $xs)"
}

puts "SLICE:   [range_of {SITE_TYPE == SLICEL || SITE_TYPE == SLICEM}]"
puts "DSP48E1: [range_of {SITE_TYPE == DSP48E1}]"
puts "RAMB18:  [range_of {SITE_TYPE == RAMB18E1}]"
puts "RAMB36:  [range_of {SITE_TYPE == RAMB36E1}]"
puts "PS7:     [range_of {SITE_TYPE == PS7}]"
