# run_csynth_a3_sol30_widen512.tcl -- ZHR-92 angle-B follow-up (2026-08-24):
# test whether m_axi_max_widen_bitwidth is the real root cause behind all
# three burst-inference failures on this design (WRITEOUT, PW_PATCH_HOIST,
# gmem_w weight reads). No source change -- CLEAN baseline source (no
# memcpy), config_interface only.
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution30" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

config_interface -m_axi_max_widen_bitwidth 512

csynth_design
puts ">>> Done. Report: ${proj_name}/solution30/syn/report/mac_array_top_csynth.rpt"
exit
