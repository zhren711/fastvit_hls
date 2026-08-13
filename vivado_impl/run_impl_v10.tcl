# run_impl_v10.tcl — Generate BD + Run Implementation + Bitstream for dwconv_ip v10.0
# 在 Vivado Tcl Console 里执行:
#   source E:/codes/microzed/fastvit_hls/vivado_impl/run_impl_v10.tcl

puts ">>> Step 1: Generate Block Design output products..."
generate_target all [get_files fastvit_bd.bd]

puts ">>> Step 2: Launch Synthesis..."
launch_runs synth_1 -jobs 4
wait_on_run synth_1
puts "Synthesis done: [get_property STATUS [get_runs synth_1]]"

puts ">>> Step 3: Launch Implementation..."
launch_runs impl_1 -jobs 4
wait_on_run impl_1
puts "Implementation done: [get_property STATUS [get_runs impl_1]]"

puts ">>> Step 4: Generate Bitstream..."
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1

set bit_file [glob -nocomplain [get_property DIRECTORY [get_runs impl_1]]/*.bit]
if {[llength $bit_file] > 0} {
    puts ">>> SUCCESS: Bitstream at $bit_file"
    puts ">>> WNS: [get_property STATS.WNS [get_runs impl_1]]"
} else {
    puts ">>> ERROR: No bitstream generated, check timing report"
}
