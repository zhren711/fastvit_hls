set proj_name  "row_hoist_probe_proj"
set top_func   "row_hoist_probe_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files row_hoist_probe/row_hoist_probe.cpp -cflags "-std=c++14 -I."
add_files -tb row_hoist_probe/row_hoist_probe_tb.cpp -cflags "-std=c++14 -I."

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

csim_design
puts ">>> csim done."
exit
