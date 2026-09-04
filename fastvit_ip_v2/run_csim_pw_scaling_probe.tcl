set proj_name "pw_scaling_probe_csim"
open_project -reset $proj_name
set_top mac_array_top
add_files mac_array_raster_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb pw_scaling_probe_tb.cpp -cflags "-std=c++14"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csim_design
puts ">>> csim done."
exit
