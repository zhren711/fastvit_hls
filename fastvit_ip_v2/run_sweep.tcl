#================================================================
# run_sweep.tcl -- Phase A round 3: MAC_UNROLL_FACTOR resource sweep
# (1/64/128/512), csynth only, no P&R (ZHR-8 2026-08-16 direction).
#
# Each factor gets its OWN fresh project (open_project -reset) rather than
# reusing one project with per-solution cflags, specifically to avoid any
# risk of a solution silently inheriting a stale -D value from a previous
# iteration -- a subtle scoping bug here would produce four "different"
# results that are secretly identical, exactly the kind of unrepresentative
# number this round exists to rule out.
#================================================================

set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

foreach factor {1 64 128 512} {
    set proj_name "mac_array_poc_u${factor}"
    puts ">>> ================= MAC_UNROLL_FACTOR=${factor} ================="
    open_project -reset $proj_name
    set_top $top_func

    add_files mac_array.cpp -cflags "-std=c++14 -DMAC_UNROLL_FACTOR=${factor}"
    add_files -tb mac_array_tb.cpp -cflags "-std=c++14 -DMAC_UNROLL_FACTOR=${factor}"

    open_solution "solution1" -flow_target vivado
    set_part $part
    create_clock -period $clk_period -name default

    csynth_design
    puts ">>> Done factor=${factor}. Report: ${proj_name}/solution1/syn/report/mac_array_top_csynth.rpt"
}

puts ">>> Sweep complete: 4 projects (mac_array_poc_u1/u64/u128/u512), one csynth report each."
exit
