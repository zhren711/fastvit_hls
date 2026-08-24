# run_bitstream_mac_array_a3_sol27_dwflat.tcl -- ZHR-92, 2026-08-24: bitstream
# for the run_layer rewrite stage 4 round -- DW's flat pipeline (PW's is
# already board-verified, sol26/a3wp24). X0-120 pblock, route_design alone
# gave WNS=+0.003606ns -- razor-thin, no phys_opt needed but flagged
# explicitly: this is a bare pass, not a comfortable margin (PW's own
# round closed at +0.141ns, ~39x more slack). Critical path this round is
# the FSM-state -> shared-multiplier signature (ap_CS_fsm_reg[111] ->
# mul_32s_32s_32_2_1's DSP cascade), not gmem_meta's AXI FIFO -- a
# different bottleneck than PW's own round surfaced.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/a3wp25
set dcp_in   "$proj_dir/a3wp25.runs/impl_1/mac_array_bd_wrapper_routed.dcp"
set bit_out  "$proj_dir/a3wp25.runs/impl_1/mac_array_bd_wrapper_dwflat.bit"

puts ">>> Opening routed checkpoint: $dcp_in"
open_checkpoint $dcp_in

puts ">>> Writing bitstream (.bit + .bin)..."
write_bitstream -force -bin_file $bit_out

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -delay_type max]]
puts ""
puts ">>> WNS at bitstream time: $wns ns"
puts ">>> Bitstream: $bit_out"
