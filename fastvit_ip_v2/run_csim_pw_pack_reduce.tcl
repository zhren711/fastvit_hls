set proj_name "pw_pack_reduce_proj"
open_project -reset $proj_name
set_top pw_reduce_packed
add_files pw_pack_reduce.cpp -cflags "-std=c++14"
add_files -tb pw_pack_reduce_tb.cpp -cflags "-std=c++14"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csim_design
puts ">>> csim done."
exit
