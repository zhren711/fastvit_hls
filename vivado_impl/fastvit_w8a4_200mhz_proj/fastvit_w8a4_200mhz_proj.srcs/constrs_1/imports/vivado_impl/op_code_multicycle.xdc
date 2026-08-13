# op_code_multicycle.xdc — declares op_code_read_reg as a quasi-static
# control signal to Vivado's static timing analysis, instead of trying
# to fix the physical routing distance of its fanout (which Tier A
# proved structurally impossible: the real bottleneck cells are created
# by opt_design itself and don't exist beforehand, so no pre-constraint
# can target them).
#
# WHY THIS IS SAFE (verified reasoning, not just "try it and see"):
# op_code_read_reg is an AXI-Lite control register written by software
# BEFORE ap_start is asserted, and the existing (unchanged) driver
# protocol -- used by every op in this design already -- writes op_code
# once, triggers ap_start, waits for ap_done, THEN writes the next
# op_code. This is a hardware/software co-design invariant that already
# holds for the current, working v1.2/v2.0 designs; nothing about this
# constraint changes that software protocol. Because op_code is a
# switch(op_code) selector evaluated ONCE at C-level (not inside any
# loop), HLS's downstream per-FSM-state predicate registers
# (ap_predicate_pred*_state*) are all just re-registered copies of the
# SAME stable value at different points in the FSM's walk -- none of
# them are ever used before the FSM has progressed at least a few
# cycles past ap_start (there's always AXI-Lite settle + ap_start
# handshake overhead first). A multicycle setup multiplier of 8 (40ns
# at 200MHz) is enormously conservative relative to real op durations
# (every real layer runs for many hundreds to thousands of cycles) --
# the risk of UNDER-estimating is what would cause a functional bug
# (capturing a value before it's settled); over-estimating has no
# downside (it does not add real latency to the datapath, it only
# tells the STA tool it doesn't need to force single-cycle closure on
# this specific class of path).
#
# The corresponding -hold constraint (multiplier-1 = 7) is the standard
# Xilinx-recommended pairing (UG906) to avoid introducing a spurious
# hold violation when relaxing a setup multicycle path.
#
# KNOWN RESIDUAL RISK (no RTL/cosim simulation environment exists in
# this project to formally verify this before hardware testing -- see
# project notes): before trusting this on the full inference pipeline,
# bring up incrementally (single op_code at a time, verify output
# against known-good reference) rather than jumping straight to a full
# network run, matching how this project handled the CONV_TN=4 timing
# margin incident previously.

set_multicycle_path 8 -setup -from [get_cells -hierarchical -filter {NAME =~ *op_code_read_reg*}]
set_multicycle_path 7 -hold  -from [get_cells -hierarchical -filter {NAME =~ *op_code_read_reg*}]
