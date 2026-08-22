# run_bin_mac_array_a3.tcl -- regenerate write_bitstream with -bin_file so
# fpgautil (Zynq sysfs fpga_manager loader on the board) can consume it --
# plain .bit lacks the header strip the on-board loader needs. Reopens the
# already-phys_opt'd checkpoint, no re-route/re-opt needed.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/mac_array_a3_proj
open_checkpoint "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_phys_opt.dcp"
write_bitstream -force -bin_file "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper.bit"
puts ">>> Done: mac_array_bd_wrapper.bit + .bin"
