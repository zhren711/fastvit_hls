# pre_opt_fanout_w8a4.tcl — STEPS.OPT_DESIGN.TCL.PRE hook for
# run_impl_w8a4_200mhz_fanoutfix.tcl (Tier A of the Phase 3 200MHz plan).
#
# Unlike the W8A8-era MAX_FANOUT attempts (both of which targeted a
# WILDCARD pattern chosen before ever seeing a real netlist -- one hit
# the wrong node entirely, the other hit real cells that just weren't
# the actual bottleneck), this targets the EXACT, already-diagnosed
# cell from THIS SPECIFIC W8A4 200MHz run's own report_timing output:
#
#   fastvit_bd_i/fastvit_ip_0/inst/gmem1_m_axi_U/load_unit_0/fifo_rreq/
#   U_fifo_srl/mem_reg[5][0]_srl6_i_9
#
# This LUT6's output net has fanout=203 and alone contributes 1.916ns
# of route delay (31% of the total 6.214ns route delay on the worst
# path) -- the single biggest hop in the path. Baseline (no fix):
# WNS=-2.562842ns.
#
# This script REUSES the already-completed synth_1 run (does not
# re-synthesize) so the cell name is guaranteed to still exist exactly
# as diagnosed -- see run_impl_w8a4_200mhz_fanoutfix.tcl, which resets
# and reruns only impl_1 against the existing synth_1 netlist.

set cells [get_cells -hierarchical -filter {NAME =~ *mem_reg\[5\]\[0\]_srl6_i_9*} -quiet]
set n [llength $cells]
puts ">>> pre_opt_fanout_w8a4.tcl: found $n matching cell(s)"
if {$n > 0} {
    foreach c $cells { puts "    -> $c" }
    set_property MAX_FANOUT 16 $cells
    puts ">>> pre_opt_fanout_w8a4.tcl: applied MAX_FANOUT=16 to $n cell(s)"
} else {
    puts ">>> pre_opt_fanout_w8a4.tcl: WARNING -- no matching cells found, constraint not applied"
}
