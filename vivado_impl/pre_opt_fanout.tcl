# pre_opt_fanout.tcl — STEPS.OPT_DESIGN.TCL.PRE hook for run_impl_200mhz_fanoutfix2.tcl
#
# Runs BEFORE opt_design. Sets MAX_FANOUT on the actual bottleneck registers
# so opt_design's own fanout-aware optimization replicates them into multiple
# physically-local copies (one per consumer region) instead of leaving a
# single register driving a huge, physically-spread net.
#
# v1 of this script (2026-07-31) targeted op_code_read_reg and measured ZERO
# effect (WNS -2.872987ns vs baseline -2.873ns) -- report_timing afterwards
# showed op_code_read_reg's own Q-output net has fanout=1 (fo=1, routed);
# MAX_FANOUT had nothing to do there. The real high-fanout nodes are one
# level downstream: registered per-FSM-state predicate comparators named
# ap_predicate_pred<N>_state<M> (confirmed in fastvit_ip_proj/solution1/syn/
# verilog/fastvit_ip.v, e.g. "ap_predicate_pred1820_state155 <= (op_code_
# read_reg_4215 == 32'd1);" -- synthesized fresh per state, but Vivado's own
# report_timing on the ORIGINAL (unfixed) 200MHz run named the actual worst
# path node "ap_predicate_pred1822_state156_reg" with fanout 213, feeding
# into gmem3_m_axi_U/store_unit_0/fifo_wreq/U_fifo_srl and crossing over into
# gmem0_m_axi_U/load_unit_0/fifo_rreq -- so this filter targets THOSE.
#
# Deliberately done at opt_design time (pre-placement) rather than via a
# post-route phys_opt_design -directive AggressiveFanoutOpt patch, which
# was tried twice before and crashed with no diagnostic output both times
# (see run_fanout_experiment*.log) -- even though it DID show a real
# partial improvement (WNS -2.873ns -> -2.303ns) before crashing. Setting
# the constraint this early gives the placer maximum freedom to place each
# replica near its actual consumers, which a post-route patch cannot do
# (placement is already fixed by then).

set cells [get_cells -hierarchical -filter {NAME =~ *ap_predicate_pred*} -quiet]
set n [llength $cells]
puts ">>> pre_opt_fanout.tcl: found $n ap_predicate_pred* cell(s)"
if {$n > 0} {
    set_property MAX_FANOUT 8 $cells
    puts ">>> pre_opt_fanout.tcl: applied MAX_FANOUT=8 to $n cell(s)"
} else {
    puts ">>> pre_opt_fanout.tcl: WARNING -- no ap_predicate_pred* cells found, constraint not applied (check cell naming)"
}
