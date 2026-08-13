open_project pwconv_ip_proj
set_top pwconv_ip
add_files pwconv_ip.cpp -cflags "-std=c++14"
add_files pwconv_ip.h   -cflags "-std=c++14"
open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 5 -name default
csynth_design
export_design -format ip_catalog -display_name "FastVIT_PWConv_IP_v3" \
              -description "256 MAC/cycle PW Conv (TM=16,TN=16,TS=32)" \
              -vendor "user.org" -version "1.0"
puts ">>> pwconv_ip v3 done."
close_project
