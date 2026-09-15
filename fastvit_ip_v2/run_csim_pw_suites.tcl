# run_csim_pw_suites.tcl -- ZHR-92 (2026-09-15): the two PW testbenches
# (pw_weight_hoist_tb.cpp, 6 real-shape cases incl. chunked layers;
# pw_scaling_probe_tb.cpp, 20 synthetic PW shapes) in one session. These are
# the ONLY csim coverage of PW_FLAT / FAST_WRITEOUT -- the four "standard"
# suites (raster, wiring, gelu_add, scalar_ops) never touch PW. Both had
# been on the 9-arg pre-ELEMWISE_BURST mac_array_top() signature since
# 2026-09-06 and were re-paired this round. Pass extra cflags via the
# CSIM_CFLAGS env var (e.g. -DPW_DEFER_WRESP); empty = default build.
set extra ""
if {[info exists ::env(CSIM_CFLAGS)]} { set extra $::env(CSIM_CFLAGS) }
set cf "-std=c++14 $extra"
foreach {proj tb} {pw_hoist_csim pw_weight_hoist_tb.cpp pw_scaling_csim pw_scaling_probe_tb.cpp} {
    open_project -reset $proj
    set_top mac_array_top
    add_files mac_array_raster_integrated.cpp -cflags $cf
    add_files dw_raster_layer.cpp -cflags $cf
    add_files -tb $tb -cflags $cf
    open_solution "s1" -flow_target vivado
    set_part "xc7z020clg400-1"
    create_clock -period 10 -name default
    csim_design
    close_project
}
exit
