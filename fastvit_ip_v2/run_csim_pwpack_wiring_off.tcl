set proj_name "pwpack_wiring_off_proj"
open_project -reset $proj_name
set_top mac_array_top
add_files mac_array_raster_pwpack_integrated.cpp -cflags "-std=c++14"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files pw_pack_pipeline.cpp -cflags "-std=c++14"
add_files -tb mac_array_raster_integrated_wiring_tb.cpp -cflags "-std=c++14"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csim_design
puts ">>> csim done."
exit
