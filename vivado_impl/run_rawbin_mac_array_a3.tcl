# run_rawbin_mac_array_a3.tcl -- A3 board bring-up (ZHR-92, 2026-08-21):
# regenerate with -raw_bin_file. dmesg on the real board (root@192.168.1.50)
# showed the plain -bin_file output failing with "Invalid bitstream, could
# not find a sync word. Bitstream must be a byte swapped .bin file" -- the
# already-working golden fastvit_bd_wrapper.bin loads fine through this
# same zynq-fpga sysfs driver, confirming the driver just needs the
# byte-swapped raw format, not a broken board/driver. -raw_bin_file is
# Vivado's documented option for exactly this format. No re-route/re-opt
# needed, reopens the already-phys_opt'd checkpoint.
set_param general.maxThreads 1
set proj_dir [file normalize [file dirname [info script]]]/mac_array_a3_proj
open_checkpoint "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper_phys_opt.dcp"
write_bitstream -force -raw_bin_file "$proj_dir/mac_array_a3_proj.runs/impl_1/mac_array_bd_wrapper.bit"
puts ">>> Done: raw byte-swapped .bin written"
