#================================================================
# run_csim_pack_exhaustive.tcl — exhaustive brute-force csim of the
# dsp_packed_mac2 packing math alone (all 256^3 combos).
#================================================================

set proj_name "dsp_pack_exhaustive_proj"
set top_func  "dsp_packed_mac2_top"

open_project $proj_name
set_top $top_func
add_files dsp_pack_design.cpp -cflags "-std=c++14"
add_files dsp_pack_design.h   -cflags "-std=c++14"
add_files -tb tb_dsp_pack_exhaustive.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 7 -name default

puts ">>> Running exhaustive brute-force csim of dsp_packed_mac2..."
csim_design

close_project
