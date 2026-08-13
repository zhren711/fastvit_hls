# diag_check_opt_stage.tcl — quick, cheap diagnostic (no re-synth/impl):
# open the post-opt_design checkpoint from the last (-2.573ns) impl_1
# run and check whether the high-fanout cell/net names found in the
# ROUTED report_timing already exist at the OPT stage, and if not,
# find their actual names there. This tells us which TCL.PRE/POST hook
# stage is safe to target before spending another 15-20min P&R cycle.

set dcp "E:/codes/microzed/fastvit_hls/vivado_impl/fastvit_w8a4_200mhz_proj/fastvit_w8a4_200mhz_proj.runs/impl_1/fastvit_bd_wrapper_opt.dcp"
open_checkpoint $dcp

foreach pat {
    {*gmem0_m_axi_U*push*}
    {*gmem1_m_axi_U*push*}
    {*gmem1_m_axi_U*empty_n*}
    {*grp_conv_worker_fu_3967*mem_reg*srl32*}
    {*grp_pwconv_worker_fu_2121*lopt*}
} {
    set cells [get_cells -hierarchical -filter "NAME =~ \"$pat\"" -quiet]
    puts ">>> pattern '$pat' -> [llength $cells] cell(s)"
    foreach c $cells { puts "    -> $c" }
}

puts ">>> --- nets matching same patterns (in case these are net-only, not cell names) ---"
foreach pat {
    {*gmem0_m_axi_U*push*}
    {*gmem1_m_axi_U*push*}
} {
    set nets [get_nets -hierarchical -filter "NAME =~ \"$pat\"" -quiet]
    puts ">>> net pattern '$pat' -> [llength $nets] net(s)"
    foreach n $nets { puts "    -> $n" }
}
