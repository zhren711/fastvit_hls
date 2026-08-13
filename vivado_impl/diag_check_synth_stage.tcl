open_checkpoint "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_w8a4_200mhz_proj/fastvit_w8a4_200mhz_proj.runs/synth_1/fastvit_bd_wrapper.dcp"
foreach np {
    {fastvit_bd_i/fastvit_ip_0/inst/gmem0_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
    {fastvit_bd_i/fastvit_ip_0/inst/gmem1_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}
} {
    set n [get_nets -quiet $np]
    if {$n eq ""} {
        puts ">>> NOT FOUND at synth stage: $np"
    } else {
        puts ">>> FOUND at synth stage: $np  (fanin pins: [llength [get_pins -of_objects $n -filter {DIRECTION==IN}]])"
    }
}
