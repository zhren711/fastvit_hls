# pre_opt_fanout_w8a4_step2.tcl — STEPS.OPT_DESIGN.TCL.PRE hook, round 2
# (fixed). Round 2's first attempt used `get_cells` against "push_0"-
# style names and matched 0 cells every time -- confirmed via a cheap
# diagnostic on the surviving fastvit_bd_wrapper_opt.dcp checkpoint
# that push_0 is a NET (the FIFO's write-enable, fanning out to the CE
# pin of every SRLC32E bit-slice cell in the 68-deep request FIFO), not
# a cell -- hence the silent no-op and identical -2.573282ns result
# both times. This version targets `get_nets` directly and applies
# MAX_FANOUT to the nets (same mechanism as the proven
# pre_opt_fanout_w8a4.tcl fix, -2.562842ns -> -2.397ns, just correctly
# resolved as a net this time instead of a cell).
#
# Confirmed via diag_check_opt_stage2.tcl on the post-opt checkpoint
# that these exact hierarchical net paths exist with fanout matching
# the routed report_timing (271/289 at opt stage vs 270/295 routed --
# i.e. this fanout is inherent to synthesis, not introduced later):
#   gmem0_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0   fanout ~270
#   gmem1_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0   fanout ~289

set nets {
    {fastvit_bd_i/fastvit_ip_0/inst/gmem0_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
    {fastvit_bd_i/fastvit_ip_0/inst/gmem1_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
}

set total 0
foreach np $nets {
    set n [get_nets -quiet $np]
    if {$n eq ""} {
        puts ">>> pre_opt_fanout_w8a4_step2.tcl: NET NOT FOUND: $np"
        continue
    }
    set_property MAX_FANOUT 16 $n
    puts ">>> pre_opt_fanout_w8a4_step2.tcl: applied MAX_FANOUT=16 to net $np"
    incr total
}
puts ">>> pre_opt_fanout_w8a4_step2.tcl: applied MAX_FANOUT=16 to $total net(s) total"
