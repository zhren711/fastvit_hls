open_checkpoint "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_w8a4_200mhz_proj/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper_opt.dcp"
set net [get_nets {fastvit_bd_i/fastvit_ip_0/inst/gmem0_m_axi_U/load_unit_0/fifo_rreq/U_fifo_srl/push_0}]
puts ">>> all pins on net:"
foreach p [get_pins -of_objects $net -quiet] {
    puts "  pin=$p dir=[get_property DIRECTION $p] cell=[get_cells -of_objects $p -quiet]"
}
puts ">>> is MAX_FANOUT settable on nets? property list:"
puts [list_property -quiet $net]
