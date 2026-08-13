# run_hls_nocsim.tcl — pwconv_ip v4 (TM=8,TN=8,TS=128, 权重优先循环, 200MHz)
open_project pwconv_ip_proj
set_top pwconv_ip
add_files pwconv_ip.cpp -cflags "-std=c++14"
add_files pwconv_ip.h   -cflags "-std=c++14"
open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 7 -name default
csynth_design
export_design -format ip_catalog \
              -display_name "FastVIT_PWConv_IP_v4" \
              -description "64MAC/cycle PW Conv, weight-first (Tm,Tn,Ts), 200MHz" \
              -vendor "user.org" -version "1.0"
puts ">>> pwconv_ip v4 done."
close_project
