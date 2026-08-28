set proj_name "dw_linebuf2_fixed_proj"
open_project -reset $proj_name
set_top dw_linebuf_probe2_fixed
add_files dw_linebuf_probe2.cpp -cflags "-std=c++14 -DMAC_PD=1"
add_files -tb dw_linebuf_probe2_tb.cpp -cflags "-std=c++14 -DMAC_PD=1"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csynth_design
puts ">>> csynth done. Report: ${proj_name}/sol/syn/report/dw_linebuf_probe2_fixed_csynth.rpt"
exit
