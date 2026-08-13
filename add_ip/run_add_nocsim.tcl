open_project add_ip_proj
set_top add_ip
add_files add_ip.cpp -cflags "-std=c++14"
add_files add_ip.h   -cflags "-std=c++14"
open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 7 -name default
csynth_design
export_design -format ip_catalog -display_name "FastVIT_Add_IP" -description "Element-wise Add" -vendor "user.org" -version "1.0"
puts "Done."
close_project
