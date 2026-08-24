A3 mac_array_top bitstream -- run_layer rewrite stage 4, DW flat pipeline, X0-120 pblock
==========================================================================================

Archived 2026-08-24. See ZHR-92 (Linear) for the full round-by-round writeup. Second and
final leg of the run_layer rewrite (PW's is mac_array_a3_pwflat_2026-08-24) -- both PW
and DW now use the flat-pipeline technique. This bitstream carries BOTH.

What changed
------------
Same technique as PW's flat pipeline, one level shallower: DW_PATCH_STAGE stays a
separate per-ot staging call (unchanged, still runtime-bounded patch_r/patch_c --
reverting that to a compile-time bound was already tried and found ~5x more expensive
for K=3 layers, not reopened). dw_one_ot_flat replaces DW_BT_STAGE + DW_WT_STAGE +
RESET + run_reduce_dw + oc_tbl/oc_ch_tbl/shift_tbl + WRITEOUT_DW (previously repeated
per f as separate named regions) with one counter-driven PIPELINE loop per ot.

Compute-phase trip count is compile-time fixed at MAX_K*MAX_K=49 taps regardless of
real K, weight zero-filled for kh>=K||kw>=K (required for the flat counter's
compile-time wrap bound). This was flagged as an unmeasured risk going in -- the
analogous MAX_K choice already cost ~3x when tried for DW_WT_STAGE alone (98 vs 36
cycles at K=3).

run_reduce_dw (DW's own reduction function) is entirely removed -- no call sites left.

csim: 16/16, 0 errors, first attempt (unlike PW's stage 2, which needed a real bugfix
for a ch_off wrap-boundary mistake).

P&R: X0-120 (started there directly, not X0-96 -- the last real P&R success for this
design FAMILY needed X0-120; solution27's csynth LUT was essentially unchanged from
solution26's). Routed clean in one shot, no phys_opt needed, but WNS = +0.003606ns --
a BARE PASS, not a comfortable margin (PW's own round closed at +0.141ns, ~39x more
slack). Critical path is a DIFFERENT signature than PW's round: FSM state
(ap_CS_fsm_reg[111]) -> mul_32s_32s_32_2_1's DSP cascade -- the historical
FSM->shared-multiplier pattern from before the accumulator-rewrite era, not gmem_meta's
AXI FIFO.

Source
------
fastvit_ip_v2/mac_array.cpp: dw_one_ot_flat (new) replaces DW's old per-(ot,f) nested
loop. run_reduce_dw removed entirely.

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution27.
Vivado: vivado_impl/run_impl_mac_array_a3_sol27_dwflat.tcl, with
pblock_mac_array_pre_place_wide.tcl (X0-120).

Files in this directory
------------------------
mac_array_bd_wrapper_dwflat.bit          -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_dwflat_swapped.bin  -- byte-swapped .bin, board-loadable
                                             (pulled directly from the board's
                                             /lib/firmware after load, not locally
                                             re-derived)

Deployed to board as /lib/firmware/mac_array_bd_wrapper_dwflat.bin (md5
66c0a1d20b652bc0f279c1a05928acf8). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched.

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.003606ns, route_design alone -- no phys_opt_design needed, but razor-thin.
LUT: 32,347/53,200 (60.80%). DSP: 24/220 (10.91%). Slice: 11,645/13,300 (87.56%).

Board verification -- REGRESSION, not an improvement
-------------------------------------------------------
DW (entry5_dw, K=3, S=1): 0/196,608 mismatches, byte-exact, 104.52ms. This is a
REGRESSION vs. both prior baselines: 96.00ms (immediately-prior, post-accumulator-
rewrite state, +8.9% worse) and 70.08ms (the earlier pre-accumulator-rewrite baseline,
+49% worse). Achieved II=2 (better than PW's II=3) and lower csynth-estimated cycle
counts in principle, but real board time went the WRONG direction.

Leading explanation, matching a risk flagged explicitly before this round started (in
dw_one_ot_flat's own header comment): the compile-time-fixed 49-tap compute phase
wastes real cycles for K<7 layers. entry5_dw is K=3 -- only 9 of 49 taps are real,
40 are wasted-but-still-clocked every ot. The OLD (pre-rewrite) design used a
runtime-K-bounded GATHER_ALL_DW loop (~36 cycles measured for K=3, vs ~98 for the
MAX_K-bounded alternative that was tried and reverted once already for exactly this
reason, DW_WT_STAGE's own history). This round's flat pipeline reintroduces that same
tap-waste at the WHOLE-COMPUTE-PHASE level, not just one staging region -- and most of
the real network's DW layers are K=1 or K=3, not K=7, so this regression likely isn't
isolated to entry5_dw.

Not yet measured: whether K=7 layers (which don't waste any taps under this scheme)
see a real improvement instead, which would confirm the tap-waste explanation directly
rather than leave it as the leading (unconfirmed) hypothesis.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol27.tcl (fresh solution name if
   re-exporting; this export's component.xml vendor is xilinx.com, NOT user.org)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol27_dwflat.tcl
                  -nolog -nojournal
3. Bitstream:     vivado -mode batch -source run_bitstream_mac_array_a3_sol27_dwflat.tcl
                  -nolog -nojournal
