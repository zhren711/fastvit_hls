#================================================================
# run_csim_only.tcl — quick csim-only check (no csynth/export)
# Used to verify the WIP pwconv whole-tensor input cache before
# committing to a full synth+export run.
#================================================================

set proj_name  "fastvit_ip_csim_check"
set top_func   "fastvit_ip"
set part       "xc7z020clg400-1"
set clk_period "7"

open_project $proj_name
set_top $top_func

add_files fastvit_ip.cpp     -cflags "-std=c++14"
add_files conv_worker.cpp    -cflags "-std=c++14"
add_files dwconv_worker.cpp  -cflags "-std=c++14"
add_files pwconv_worker.cpp  -cflags "-std=c++14"
add_files add_worker.cpp     -cflags "-std=c++14"
add_files gelu_worker.cpp    -cflags "-std=c++14"
add_files -tb fastvit_ip_tb.cpp -cflags "-std=c++14"

open_solution "solution1" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

puts ">>> Running C Simulation only..."
csim_design

puts ">>> Done. Check fastvit_ip_csim_check/solution1/csim/report"
close_project
