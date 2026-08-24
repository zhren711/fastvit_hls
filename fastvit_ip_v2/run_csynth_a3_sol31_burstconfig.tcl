# run_csynth_a3_sol31_burstconfig.tcl -- ZHR-92 angle-B step 1 (2026-08-24):
# combined config_interface probe -- alignment + latency + widen + burst
# length together, matching the official manual_burst example's tcl plus
# the two burst-length knobs (also settable via config_interface, no
# pragma/source change needed -- confirmed via `config_interface -help`).
# Source untouched.
set proj_name  "mac_array_poc_a3_axi"
set top_func   "mac_array_top"
set part       "xc7z020clg400-1"
set clk_period "10"

open_project $proj_name
set_top $top_func

add_files mac_array.cpp -cflags "-std=c++14"
add_files -tb mac_array_tb.cpp -cflags "-std=c++14"

open_solution "solution31" -flow_target vivado
set_part $part
create_clock -period $clk_period -name default

config_interface -m_axi_alignment_byte_size 64 -m_axi_latency 64 \
                  -m_axi_max_widen_bitwidth 512 \
                  -m_axi_max_read_burst_length 256 \
                  -m_axi_max_write_burst_length 256

csynth_design
puts ">>> Done. Report: ${proj_name}/solution31/syn/report/mac_array_top_csynth.rpt"
exit
