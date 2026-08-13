# pblock_worker.xdc -- compact-floorplan Pblock for the isolated
# single-worker 200MHz harnesses (dwconv_only / pwconv_only). Round 3
# (placement directives only) got dwconv to ~-0.2ns and pwconv to
# -0.48ns, both route-delay-dominated (68-83%). Investigation
# (investigate_floorplan.tcl, 2026-08-07) found the placer had spread
# the whole worker_0 hierarchy across nearly the FULL device (SLICE
# X0-113, Y0-129 out of max X113/Y149) despite only 15-35% utilization,
# while DSP48E1/RAMB18E1 columns only exist within Y0-59 -- meaning a
# lot of pure-logic cells were scattered far above the Y-band where the
# DSP/BRAM-heavy compute logic must live. Bounding the whole worker
# instance (dwconv_worker_U/pwconv_worker_U + its private AXI adapters +
# the AXI4-Lite control regfiles) into a compact Pblock anchored to that
# band should shorten average wire length without cutting into needed
# resource capacity (dwconv's worst-case usage -- 34.7% LUT/23.8% FF/
# 25% BRAM/14.1% DSP -- comfortably fits the ~60%-of-device area below).
create_pblock pblock_worker
add_cells_to_pblock [get_pblocks pblock_worker] \
    [get_cells -hierarchical -filter {NAME =~ "fastvit_bd_i/worker_0/inst*"}]
resize_pblock [get_pblocks pblock_worker] \
    -add {SLICE_X0Y0:SLICE_X113Y89 DSP48_X0Y0:DSP48_X4Y59 RAMB18_X0Y0:RAMB18_X5Y59}
set_property SNAPPING_MODE ROUTING [get_pblocks pblock_worker]
