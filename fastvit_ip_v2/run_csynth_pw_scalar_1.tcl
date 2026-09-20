# run_csynth_pw_scalar_1.tcl -- ZHR-92 (2026-09-19): option (b) standalone resource probe, csynth only at 10ns.
open_project -reset probe_pw_scalar_1
set_top pw_scalar_1
add_files pw_scalar_probe.cpp -cflags "-std=c++14"
open_solution "s1" -flow_target vivado
set_part xc7z020clg400-1
create_clock -period 10 -name default
csynth_design
exit
