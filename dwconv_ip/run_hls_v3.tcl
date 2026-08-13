open_project dwconv_ip_proj
set_top dwconv_ip
add_files dwconv_ip.cpp -cflags "-std=c++14"
add_files dwconv_ip.h   -cflags "-std=c++14"
open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 5 -name default
csynth_design
export_design -format ip_catalog -display_name "FastVIT_DWConv_IP_v3" \
              -description "DW Conv TN=4" \
              -vendor "user.org" -version "1.0"
puts ">>> dwconv_ip v3 done."
close_project
