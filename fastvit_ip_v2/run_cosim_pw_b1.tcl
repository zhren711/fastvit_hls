# run_cosim_pw_b1.tcl -- ZHR-92 (2026-09-04): RTL cosimulation of the FULL
# mac_array_top design (mac_array_raster_integrated.cpp + dw_raster_layer.cpp,
# the current deployed architecture) against a single real shape
# (pwburst_b1_t16, Group B's smallest point). First pass: cycle-count only,
# no trace (faster) -- answers "does cosim's own reported cycle count match
# the real board's, or the pure analytical prediction?" without needing
# waveforms yet.
set proj_name "pw_cosim_b1_proj"
open_project -reset $proj_name
set_top mac_array_top
add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DCOSIM_DEPTH_HINT"
add_files dw_raster_layer.cpp -cflags "-std=c++14"
add_files -tb pw_cosim_b1_tb.cpp -cflags "-std=c++14"
open_solution "sol" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csynth_design
cosim_design -rtl verilog
puts ">>> cosim done."
exit
