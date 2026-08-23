set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

open_solution "solution19"
set_part $part
create_clock -period $clk_period -name default

csynth_design
export_design -flow syn -rtl verilog -format ip_catalog -vendor user.org -library hls -version 1.0

puts ">>> Done. IP at: ${proj_name}/solution19/impl/ip"
exit
