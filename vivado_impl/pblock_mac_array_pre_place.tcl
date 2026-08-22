# pblock_mac_array_pre_place.tcl -- ZHR-92, 2026-08-22, option E: constrain
# the whole mac_array_top_0 IP instance (which contains BOTH gmem_meta's
# AXI interface logic and run_layer's registers) to a compact physical
# region instead of letting the placer spread it across the full device.
# Diagnosis (corrected from option D's wrong "high fanout" theory): the
# first P&R run's own record said "zero logic levels, 74% routing delay,
# fanout=70" for gmem_meta's critical path -- that's a PLACEMENT DISTANCE
# signature (short logic path, long wire), not a fan-out signature. A
# pblock is a physical constraint targeting exactly that, unlike the
# place_design/phys_opt_design directive rotation this project already
# falsified four times (that was blind strategy-swapping; this targets a
# known, diagnosed path).
#
# Device is xc7z020clg400-1: full SLICE range X0-113/Y0-149 (queried
# directly via query_device_bounds.tcl, not assumed). Real LUT need for
# this build is 66.53% (35,393/53,200) -- using ~85% of the X range keeps
# real headroom while still meaningfully compacting vs. the full width.
# RAMB18/RAMB36/DSP48 left at full range (not the bottleneck resource,
# restricting them too adds failure risk for no benefit).
create_pblock pblock_mac_array
add_cells_to_pblock [get_pblocks pblock_mac_array] \
    [get_cells -hierarchical -filter {NAME =~ "*mac_array_top_0/inst*"}]
resize_pblock [get_pblocks pblock_mac_array] -add \
    {SLICE_X0Y0:SLICE_X96Y149 RAMB18_X0Y0:RAMB18_X5Y59 RAMB36_X0Y0:RAMB36_X5Y29 DSP48_X0Y0:DSP48_X4Y59}
set_property CONTAIN_ROUTING true [get_pblocks pblock_mac_array]
puts ">>> pblock_mac_array created: [llength [get_cells -of_objects [get_pblocks pblock_mac_array]]] cells"
