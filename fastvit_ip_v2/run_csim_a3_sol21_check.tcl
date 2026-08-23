set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution21" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csim_design
puts ">>> csim done."
exit
