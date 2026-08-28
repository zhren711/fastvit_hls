set proj_name  "dw_linebuf_real_tile_proj"
open_project -reset $proj_name
set_top mac_array_top
add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb dw_linebuf_real_tile.cpp -cflags "-std=c++14"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csim_design
puts ">>> csim done."
exit
