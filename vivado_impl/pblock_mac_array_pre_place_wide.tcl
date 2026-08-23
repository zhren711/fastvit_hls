# pblock_mac_array_pre_place_wide.tcl -- ZHR-92, 2026-08-22, option E
# followup: widened from X0-96 to X0-120 (clamps to the device's real max,
# X113 -- effectively full width) after the DW whole-block-burst design
# failed to ROUTE (not place) inside the original X0-96 pblock. Root cause:
# Win_reg's fanout grew (now drives the burst base-address computation, the
# w==Win-1 wraparound counter, and both the fast/edge read paths), and the
# original pblock was sized for a design that didn't have this net -- the
# constraint didn't follow the design change. Real LUT was already
# confirmed unchanged (67.03% vs 66.55%) before this widening -- this is a
# routing-congestion fix, not a capacity fix, so only the X range grows,
# not the RAMB/DSP ranges (still not the bottleneck resource).
create_pblock pblock_mac_array
add_cells_to_pblock [get_pblocks pblock_mac_array] \
    [get_cells -hierarchical -filter {NAME =~ "*mac_array_top_0/inst*"}]
resize_pblock [get_pblocks pblock_mac_array] -add \
    {SLICE_X0Y0:SLICE_X120Y149 RAMB18_X0Y0:RAMB18_X5Y59 RAMB36_X0Y0:RAMB36_X5Y29 DSP48_X0Y0:DSP48_X4Y59}
set_property CONTAIN_ROUTING true [get_pblocks pblock_mac_array]
puts ">>> pblock_mac_array created: [llength [get_cells -of_objects [get_pblocks pblock_mac_array]]] cells"
