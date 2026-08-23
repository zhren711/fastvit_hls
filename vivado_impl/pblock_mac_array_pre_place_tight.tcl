# pblock_mac_array_pre_place_tight.tcl -- ZHR-92, 2026-08-23, accumulator
# rewrite round: TIGHTENING attempt (X0-120 -> X0-96), the opposite
# direction from the two prior LUT-shift rounds.
#
# Pattern seen three times now, all after a design SHRANK inside the
# unchanged X0-120 pblock:
#   DW-burst (grew):        LUT 66.55%->67.03%, widened X0-96->X0-120,
#                            fixed a ROUTING FAILURE (not a timing miss).
#   staging optimization:   LUT 66%->55.07%, kept X0-120, -0.003ns ->
#                            widening to X0-120 (already done) -> +0.113ns.
#   accumulator rewrite:    LUT 59.09%->57.26%, X0-120 unchanged, route
#                            delay share 40.4%->51.4%, WNS -0.122->-0.451ns.
#
# The middle round's own fix ("widen further") always felt backwards for a
# SHRINKING design -- the more likely truth: X0-120 is now too LOOSE for a
# smaller design, letting the placer spread cells out and lengthen the
# routes between the two ends of the critical path. This round tests that
# directly by tightening back to X0-96 (the pre-DW-burst size, LUT 66%/
# WNS +0.165ns at the time) instead of widening again. Judged on TWO
# criteria together, not WNS alone: (1) route_design WNS>=0, (2) the
# critical path's route-delay SHARE drops back toward ~40% (currently
# 51.4%) -- that ratio is the direct signal for "was the design spread out
# by too much room," independent of whether WNS alone happens to clear 0.
#
# RAMB18/RAMB36/DSP48 ranges unchanged (never the bottleneck resource in
# any of these three rounds).
create_pblock pblock_mac_array
add_cells_to_pblock [get_pblocks pblock_mac_array] \
    [get_cells -hierarchical -filter {NAME =~ "*mac_array_top_0/inst*"}]
resize_pblock [get_pblocks pblock_mac_array] -add \
    {SLICE_X0Y0:SLICE_X96Y149 RAMB18_X0Y0:RAMB18_X5Y59 RAMB36_X0Y0:RAMB36_X5Y29 DSP48_X0Y0:DSP48_X4Y59}
set_property CONTAIN_ROUTING true [get_pblocks pblock_mac_array]
puts ">>> pblock_mac_array created: [llength [get_cells -of_objects [get_pblocks pblock_mac_array]]] cells"
