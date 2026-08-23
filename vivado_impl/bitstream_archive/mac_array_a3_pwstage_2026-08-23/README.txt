A3 mac_array_top bitstream -- PW_STAGE elimination + accumulator rewrite, X0-96 pblock
=========================================================================================

Archived 2026-08-23. See ZHR-92 (Linear) for the full round-by-round writeup. This is
the LAST KNOWN-GOOD state on this line, after:

1. PW_STAGE elimination: run_reduce_unified's PW read path made to read
   pw_patch_full directly (fully ARRAY_PARTITIONed), removing the
   per-(rt,colt,ot,cbase) copy into a smaller chunk-local buffer that was
   ~71% of PW's total latency. Found and fixed a real csynth-side
   duplicate-instance bug along the way (a DW-branch dummy array's shape
   mismatch broke cross-call-site resource sharing, DSP 70->102) -- fixed
   by reusing the same real object at both call sites.
2. Accumulator rewrite: five `index * runtime-stride` address computations
   (DW_PATCH_STAGE, DW_WT_STAGE, WRITEOUT_DW, WRITEOUT_PW, PW_PATCH_HOIST)
   converted from per-iteration multiplies to loop-carried accumulators,
   targeting a critical path (FSM state -> mul_32s_32s_32_2_1's DSP
   cascade register) that persisted across three independent rounds.
   Reduced logic delay as intended (5.123ns->4.905ns) but a second-order
   placement effect (design shrank inside the unchanged X0-120 pblock,
   letting cells spread out) made ROUTE delay worse (3.466ns->5.190ns),
   netting WNS -0.122ns->-0.451ns.
3. pblock tightened X0-120->X0-96 (NOT widened -- direction determined by
   the critical path's route-delay SHARE growing, 40.4%->51.4%, not by
   grow/shrink direction alone). Fixed it cleanly: WNS +0.112ns, 0 Setup
   failing endpoints, route-delay share back to 38.3%.
4. A follow-up attempt to also hoist WRITEOUT_DW's per-call oc_tbl/oc_ch_tbl
   lookup table to an outer, once-per-run_layer-call form was ATTEMPTED AND
   REVERTED same day -- csim clean, but grew the design enough to fail P&R
   routing at X0-96 (83 unroutable pins). The premise behind it (WRITEOUT_DW
   itself, or its own repeated evaluation, explaining the accumulator
   rewrite's board-measured DW regression) was also independently refuted:
   every named region's own csynth report is identical or IMPROVED between
   the pre- and post-rewrite versions. The real +422 cycles/tile source is
   confirmed to be in run_layer's unlabeled sequential glue code (see
   CLAUDE.md's new csynth-blind-spot note) -- not yet isolated further,
   deliberately not chased past this point (see the line-closure decision
   below).

Source
------
fastvit_ip_v2/mac_array.cpp: run_reduce_unified's PW parameter is now
pw_patch_full[MAX_CIN] + pw_c0 (was a MAX_CIN_PW-deep chunk copy). PW_STAGE
is gone. DW_PATCH_STAGE/DW_WT_STAGE/WRITEOUT_DW/WRITEOUT_PW/PW_PATCH_HOIST
all use loop-carried accumulators (ch_off, wt_base+wt_step, oc_tbl/oc_ch_tbl
tables, ot_out_ch_base, in_ch_base) instead of per-iteration `index*stride`
multiplies.

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution21.
Vivado: vivado_impl/run_impl_mac_array_a3_sol21_tight.tcl, with
pblock_mac_array_pre_place_tight.tcl (X0-96 -- tightened FROM X0-120, the
opposite direction of the two prior LUT-shift rounds; see CLAUDE.md).

Files in this directory
------------------------
mac_array_bd_wrapper_pwstage.bit          -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_pwstage_swapped.bin  -- byte-swapped .bin, board-loadable
                                              (pulled directly from the board's
                                              /lib/firmware after load, not
                                              locally re-derived -- see the
                                              merge-round README for why)

Deployed to board as /lib/firmware/mac_array_bd_wrapper_pwstage.bin (md5
fc759333703ee2579a803242b17017a7). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.112ns, route_design alone -- no phys_opt_design needed. 0 Setup
failing endpoints. LUT: 30,469/53,200 (57.27%). DSP: 59/220 (26.82%). BRAM:
21/140 (15.00%). Critical path still the same FSM-state-driven shared
multiplier signature as the two prior rounds (mul_32s_32s_32_2_1, same
physical instance) -- not eliminated, just enough margin to meet timing.

Board verification
-------------------
PW  (entry3, wide-eligible shape): 0/196,608 mismatches, byte-exact, 133.74ms
    (down from 171.76ms pre-line, -22%; pre-registered target was ~50ms --
    the PW_STAGE csynth cycle estimate did not predict board time accurately,
    see CLAUDE.md's csynth-blind-spot note)
DW  (entry5_dw, K=3 S=1): 0/196,608 mismatches, byte-exact, 96.00ms (UP from
    70.08ms pre-accumulator-rewrite, +37% -- root cause confirmed to be in
    run_layer's sequential glue code, NOT any named pipeline region; see
    CLAUDE.md and ZHR-92 for the full investigation)

Line closed here, decision to stop
------------------------------------
Net effect of the last three rounds (PW_STAGE elimination, accumulator
rewrite, pblock tighten) is roughly a wash: PW -22%, DW +37%, comparable
magnitude in opposite directions. Meanwhile two single layers have been
tuned in isolation and the real 82-entry network has never run end-to-end
on this architecture. Pivoting to: board-validate the remaining untested
ops (GELU, GAP, SE gating flow) individually, then run the full 82-entry
network for the first time on this architecture, rather than continuing to
chase the glue-logic cost (which would need RTL cosim -- unused anywhere in
this project so far -- to isolate further).

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol21.tcl (fresh solution
   name if re-exporting -- verify hdl/verilog freshness per CLAUDE.md's
   export_design staleness note)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol21_tight.tcl
                  -nolog -nojournal
3. Bitstream:     vivado -mode batch -source run_bitstream_mac_array_a3_sol21_tight.tcl
                  -nolog -nojournal   (writes .bit + .bin; the board's own
                  /home/root/fpga/fpga_overlay.py Overlay class does its own
                  byte-swap + /lib/firmware deploy from the raw .bit)
