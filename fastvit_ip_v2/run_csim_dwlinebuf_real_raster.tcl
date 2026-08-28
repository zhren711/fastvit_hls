set proj_name  "dw_linebuf_real_raster_proj"
open_project -reset $proj_name
set_top dw_linebuf_probe2
add_files dw_linebuf_probe2.cpp -cflags "-std=c++14 -DMAC_PD=48"
add_files -tb dw_linebuf_real_raster.cpp -cflags "-std=c++14 -DMAC_PD=48"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csim_design
puts ">>> csim done."
exit
