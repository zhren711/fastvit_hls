A3 mac_array_top bitstream -- gmem_act_wide/gmem_act master merge
====================================================================

Archived 2026-08-23. See ZHR-92 (Linear) for the full round writeup: the
prior round (solution18) added a 5th AXI master (gmem_act_wide) for
PW_PATCH_HOIST's wide read, which failed timing (WNS -0.292ns). P&R's own
timing record showed the critical path was NOT gmem_meta-related (Option
E's old placement-distance signature) but internal to run_layer -- an FSM
state bit driving a 32x32 multiplier's DSP cascade register, 4 logic
levels, 58% logic delay, matching a shared-multiplier-plus-mux structure,
not a placement problem. This round merged in_base_wide back onto
bundle=gmem_act (4 masters again) and unified PW_PATCH_HOIST's wide/narrow
branches into one word-read path (removing one of the two `ci *
d.in_ch_stride` computations HLS was plausibly sharing one multiplier
across).

Real correctness bug found and fixed via csim, not assumed: a first
single-word-read-per-(ci,rr) version crashed ("Hi(39) out of bound(32) in
range()") on Phase4 (desc4[1], a pre-existing round-9 test: PW cin=32,
w_out=10 -- a genuine partial last column tile, col_sz=2, at a
non-4-aligned offset). The "wide-eligible layers are always 4-word-aligned"
invariant only holds for the real 82-entry network, not arbitrary csim
shapes -- fixed with a general 2-word read, second word fetched only when
a valid cw actually spills into it (zero extra cost for the real-network
aligned/full-tile case).

Source
------
fastvit_ip_v2/mac_array.cpp: PW_PATCH_HOIST_WIDE/PW_PATCH_HOIST_NARROW
merged into one PW_PATCH_HOIST loop, unconditional (d.use_wide_path no
longer read here). in_base_wide's m_axi pragma bundle changed from
gmem_act_wide back to gmem_act.
fastvit_ip_v2/mac_array.h: use_wide_path field kept declared (unused,
zero-init-safe) with a comment explaining why.

HLS solution: fastvit_ip_v2/mac_array_poc_a3_axi/solution19.
Vivado: vivado_impl/run_impl_mac_array_a3_sol19_merge.tcl, same pblock as
the prior round (pblock_mac_array_pre_place_wide.tcl, X0-120) -- LUT moved
55.07%->58.85%, still small enough that the existing pblock didn't need
re-widening this round (unlike the two prior LUT-shift rounds).

Files in this directory
------------------------
mac_array_bd_wrapper_merge.bit           -- Vivado bitstream (not byte-swapped)
mac_array_bd_wrapper_merge_swapped.bin   -- byte-swapped .bin, board-loadable
                                             (this is what was deployed;
                                             pulled directly from the board's
                                             /lib/firmware after load, not
                                             locally re-derived -- a local
                                             from-scratch reimplementation of
                                             the byte-swap produced a
                                             DIFFERENT md5 despite identical
                                             size, root cause not chased down,
                                             the board's own fpga_overlay.py
                                             bit_to_bin() is the trusted
                                             source of truth here)

Deployed to board as /lib/firmware/mac_array_bd_wrapper_merge.bin (md5
66f679f82ad553d085a54aa876aee4f6). Golden rollback image
/lib/firmware/fastvit_bd_wrapper.bin was NOT touched (verified unchanged
timestamp/md5 before and after this deploy).

Timing / resources (real P&R, not csynth estimate)
----------------------------------------------------
WNS: +0.115ns, route_design alone -- no phys_opt_design needed. LUT:
31,309 / 53,200 (58.85%) -- down slightly from 59.13% (solution18, 5
masters) but still above the 55.07% pre-wide-port baseline (solution14).
DSP48E1: 70 (real P&R; down from solution18's 73, matches csynth's own
estimate exactly this round). BRAM: 13/140 (9.29%).

Critical path (STILL present, not eliminated by this merge): FSM state
reg (ap_CS_fsm_reg[64]) -> mul_32s_32s_32_2_1's DSP cascade register
(buff0_reg/PCIN), 4 logic levels, 60.5% logic / 39.5% route delay. Slack
improved enough to meet timing (+0.115ns) but the shared-multiplier/mux
structure itself persists -- diagnosis: HLS is likely still binding this
multiplier across OTHER `*_ch_stride` products in run_layer (DW_PATCH_STAGE,
WRITEOUT_DW/PW each have their own), not just the two removed this round.
Confirmed by DSP only dropping 73->70, not all the way back to the
pre-wide-port ~63 region. Real fix (loop-accumulator address instead of
`idx * stride`, in staging/writeout loops only, NOT the 512-wide unrolled
region where this was tried once before and made things worse) is a
registered TODO, not attempted this round.

Board verification
-------------------
PW  (entry3, cin=48/cout=48/h_in=w_in=64, wide-eligible shape):
    0/196,608 mismatches, byte-exact, 171.76ms (down from 178.20ms, -3.6%)
DW  (entry5_dw, K=3 S=1): 0/196,608 mismatches, byte-exact, 70.08ms
    (down from 71.24ms, -1.6%)
Layer 50/51 (SE block, narrow-path shape, w_out=1): NOT measured this
    round -- no existing board bundle covers that shape. Open item.

Why PW's improvement (6.4ms) fell far short of the pre-registered 75-95ms
"effective" band (178.20ms -> ~140ms upper bound would have been the
correct pre-registration): PW_STAGE (the on-chip BRAM-to-BRAM copy from
pw_patch_full into the per-chunk pw_patch, a SEPARATE stage from
PW_PATCH_HOIST) was already measured and documented in this file's own
comments as ~71% of PW's total 179ms (~127ms), months before this round.
PW_PATCH_HOIST -- what this round actually widened -- is the DRAM-read
stage feeding that cache, a minority contributor to begin with (~29% of
total). The pre-registration conflated two different findings: the HW
Interfaces bit-width table's "gmem_act is a real bottleneck" (a GLOBAL,
cross-cutting finding) with "PW_PATCH_HOIST is PW's bottleneck" (never
actually established -- PW_STAGE was). Corrected pre-registration would
have been: 29% * (up to 4x) = up to ~38ms max savings, PW ~140ms ceiling,
not 83ms. The observed 6.4ms savings means PW_PATCH_HOIST itself is only
a small slice of even that 29% remainder.

What this round actually delivered (not the 6.4ms):
- AXI master count back to 4 (structural correctness, matches the
  pre-wide-port master count)
- A real csim-caught bug fixed: the "wide layers are always 4-word-aligned"
  invariant only holds for the real 82-entry network, not arbitrary csim
  shapes or future model/resolution changes -- do not encode a
  real-network-only invariant as a hardware precondition without a runtime
  guard (this round's 2-word-read fallback IS that guard). General lesson,
  not specific to this one field.
- Wide-read infrastructure (in_base_wide, the m_axi bundle, the general
  mod-4 byte-lane decomposition) now exists and is verified correct --
  currently not the lever that matters for PW (PW_STAGE is), but reusable
  once PW_STAGE itself is eliminated (see next round).

Next round (accounting only, not this one): PW_STAGE elimination via full
ARRAY_PARTITION on pw_patch_full, letting run_reduce_unified read it
directly. Attempted once before (2 months ago) and reverted -- resource
conditions were BRAM 95%/LUT 95%/WNS -1.289ns (still -0.368ns after
phys_opt) at the time, a bad trade. Current conditions (BRAM 9.3%, LUT
58.85%, WNS +0.115ns) are much more favorable; the prior attempt's real
measured cost was +32 BRAM_18K, which on today's 13/140 baseline is
45/140 = 32%, well within budget. That prior attempt also taught: a
pragma-only csynth probe UNDERSOLD the real cost (BRAM 303->335 estimated
vs. the real P&R picture never even reached, since the probe alone looked
survivable and only real P&R exposed WNS -1.289ns) -- so the next round's
plan is a pragma-only change to measure LUT/BRAM, followed immediately by
a real P&R run, not a csynth-only conclusion.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_a3_sol19.tcl (fresh solution
   name if re-exporting -- verify hdl/verilog freshness per CLAUDE.md's
   export_design staleness note)
2. vivado_impl:   vivado -mode batch -source run_impl_mac_array_a3_sol19_merge.tcl
                  -nolog -nojournal
3. Bitstream:     vivado -mode batch -source run_bitstream_mac_array_a3_sol19_merge.tcl
                  -nolog -nojournal   (writes .bit + .bin; the board's own
                  /home/root/fpga/fpga_overlay.py Overlay class does its own
                  byte-swap + /lib/firmware deploy from the raw .bit -- no
                  separate local byte-swap step needed, see mac_array_driver
                  notes)
4. ARM test driver source: fastvit_ip_v2/a3_single_op_test_src/ (pulled from
   patrick@192.168.1.87:~/a3_single_op_test/ this round, not previously
   committed -- includes the in_base_wide register wiring added this round,
   offset 0x60/0x64 confirmed from solution19's generated xmac_array_top_hw.h)
