open_project conv_ip_proj
set_top conv_ip
add_files conv_ip.cpp -cflags "-std=c++14"
add_files conv_ip.h   -cflags "-std=c++14"
open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 5 -name default
csynth_design
export_design -format ip_catalog -display_name "FastVIT_Conv_IP_v3" \
              -description "Stem Conv TN=1 TM=1 (LUT-saving)" \
              -vendor "user.org" -version "1.0"
puts ">>> conv_ip v3 done."
close_project
