# run_rawbin_mac_array_a3_dw_fix.tcl -- ZHR-92, 2026-08-22: regenerate this
# round's bitstream with -bin_file (corrected flag name -- -raw_bin_file
# does not exist in this Vivado version, confirmed via write_bitstream
# -help; the earlier round's script header comment was wrong about the
# flag name). Reopens the confirmed-fresh, already-phys_opt'd checkpoint
# (WNS=+0.120ns).
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/mac_array_a3_proj
open_checkpoint "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_routed_physopt.dcp"
write_bitstream -force -bin_file "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_dw_fix.bit"
puts ">>> Done: .bit + .bin written"
