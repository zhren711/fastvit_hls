# diag_check_opt_stage2.tcl — resolve the actual DRIVER CELLS (not just
# net names) for the two highest-fanout nets found in the routed
# report_timing (gmem0/gmem1 fifo_rreq/U_fifo_srl/push_0), on the
# post-opt checkpoint, so a MAX_FANOUT fix can be applied to a real
# cell/net that exists at the stage we hook into.

set dcp "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_w8a4_200mhz_proj/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper_opt.dcp"
open_checkpoint $dcp

foreach netpath {
    {fastvit_bd_i/fastvit_ip_0/inst/gmem0_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
    {fastvit_bd_i/fastvit_ip_0/inst/gmem1_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
} {
    set net [get_nets -quiet $netpath]
    if {$net eq ""} {
        puts ">>> NET NOT FOUND: $netpath"
        continue
    }
    set driver_pin [get_pins -quiet -of_objects $net -filter {DIRECTION == OUT}]
    set driver_cell [get_cells -quiet -of_objects $driver_pin]
    set fanout [llength [get_pins -quiet -of_objects $net -filter {DIRECTION == IN}]]
    puts ">>> net: $netpath"
    puts "    driver_pin:  $driver_pin"
    puts "    driver_cell: $driver_cell"
    puts "    fanout (input pin count): $fanout"
    if {$driver_cell ne ""} {
        puts "    driver_cell REF_NAME: [get_property REF_NAME $driver_cell]"
    }
}
