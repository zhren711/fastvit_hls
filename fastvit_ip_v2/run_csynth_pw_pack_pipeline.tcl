set proj_name "pw_pack_pipeline_csynth_proj"
open_project -reset $proj_name
set_top pw_flat_pipeline_packed
add_files pw_pack_pipeline.cpp -cflags "-std=c++14"
add_files -tb pw_pack_pipeline_csynth_tb.cpp -cflags "-std=c++14"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csynth_design
puts ">>> csynth done. Report: ${proj_name}/sol/syn/report/pw_flat_pipeline_packed_csynth.rpt"
exit
