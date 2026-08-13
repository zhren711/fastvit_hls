# post_opt_fanout_w8a4_step2.tcl — STEPS.OPT_DESIGN.TCL.POST hook,
# round 2 (second fix: PRE doesn't work because these net names don't
# exist until AFTER opt_design has already restructured/renamed the
# netlist -- confirmed via diag_check_synth_stage.tcl, both target nets
# are "NOT FOUND" on the raw synth_1 checkpoint, but ARE found with
# matching fanout on the post-opt checkpoint). This hook runs right
# after opt_design completes, sets MAX_FANOUT on the real nets, then
# explicitly re-invokes `opt_design -fanout_opt` so the replication
# actually happens now (setting the property alone does nothing on an
# already-optimized netlist -- something has to consume it before
# place_design runs).

set nets {
    {fastvit_bd_i/fastvit_ip_0/inst/gmem0_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
    {fastvit_bd_i/fastvit_ip_0/inst/gmem1_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
}

set total 0
foreach np $nets {
    set n [get_nets -quiet $np]
    if {$n eq ""} {
        puts ">>> post_opt_fanout_w8a4_step2.tcl: NET NOT FOUND: $np"
        continue
    }
    set_property MAX_FANOUT 16 $n
    puts ">>> post_opt_fanout_w8a4_step2.tcl: applied MAX_FANOUT=16 to net $np (fanin pins before: [llength [get_pins -of_objects $n -filter {DIRECTION==IN}]])"
    incr total
}
puts ">>> post_opt_fanout_w8a4_step2.tcl: applied MAX_FANOUT=16 to $total net(s)."
puts ">>> post_opt_fanout_w8a4_step2.tcl: NOTE - 'opt_design -fanout_opt' is not a valid option in"
puts ">>> this Vivado version (2024.2); the MAX_FANOUT property is left in place for phys_opt_design's"
puts ">>> AggressiveFanoutOpt directive (set at the run level, see run_w8a4_200mhz_multicycle_fanoutfix3.tcl)"
puts ">>> to consume during placement/physical optimization instead."
