open_project dwconv_ip_proj
set_top dwconv_ip
add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"
open_solution "solution1" -reset -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 7 -name default
csynth_design
export_design -format ip_catalog -display_name "FastVIT_DWConv_IP_v6_DATAFLOW" -description "DW_TN=1 DATAFLOW ping-pong, 7ns" -vendor "user.org" -version "6.0"
puts "dwconv v6.0 DATAFLOW done."
close_project
