#================================================================
# export_xsa.tcl — 从 Vivado 项目导出 XSA 硬件描述文件
# 用于 Petalinux / Vitis 开发
#
# 用法: vivado -mode batch -source export_xsa.tcl -nolog -nojournal
#================================================================

set script_dir [file normalize [file dirname [info script]]]
set proj_xpr   "$script_dir/fastvit_util_check/fastvit_util_check.xpr"
set xsa_out    "$script_dir/../petalinux/hardware/fastvit_hw.xsa"

puts ">>> Opening project: $proj_xpr"
open_project $proj_xpr

puts ">>> Opening implemented design..."
open_run impl_1

puts ">>> Exporting hardware (XSA)..."
write_hw_platform -fixed -include_bit -force \
    -file [file normalize $xsa_out]

puts ""
puts "========================================"
puts " XSA exported: $xsa_out"
puts "========================================"

close_project
