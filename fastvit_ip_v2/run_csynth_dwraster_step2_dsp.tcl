set proj_name "dw_raster_step2_dsp_csynth_proj"
open_project -reset $proj_name
set_top run_dw_layer_raster
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DLB_FORCE_DSP"
add_files -tb dw_raster_layer_csynth_tb.cpp -cflags "-std=c++14 -DLB_FORCE_DSP"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csynth_design
puts ">>> csynth done. Report: ${proj_name}/sol/syn/report/run_dw_layer_raster_csynth.rpt"
exit
