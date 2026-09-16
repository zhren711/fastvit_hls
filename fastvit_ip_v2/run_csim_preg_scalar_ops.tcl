# run_csim_scalar_ops.tcl -- ZHR-92 (2026-09-12): csim for GAP/RELU/
# SIGMOID/SCALE on the raster architecture (scalar_ops_real_desc_tb.cpp,
# real desc_all.bin descriptors for entries 75/77/79/80, Python reference
# from tools/gen_scalar_ops_csim_bundle.py). First-ever csim coverage of
# these four ops on this architecture.
open_project -reset preg_scalar_csim
set_top mac_array_top
add_files mac_array_raster_integrated.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
add_files dw_raster_layer.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
add_files -tb scalar_ops_real_desc_tb.cpp -cflags "-std=c++14 -DCTR_NARROW -DPW_MAC_PREG -DPW_ACC_NARROW"
open_solution "s1" -flow_target vivado
set_part "xc7z020clg400-1"
create_clock -period 10 -name default
csim_design
exit
