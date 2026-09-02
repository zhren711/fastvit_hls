A3 mac_array_top bitstream -- PW weight residency (144KB on-chip cache)
========================================================================

Archived 2026-09-02. See ZHR-92 (Linear) for the full round-by-round writeup,
ZHR-63 for the mainline latency-line summary. Replaces the deployed baseline
(mac_array_a3_gmemmeta_elim1, 2026-08-31) as this line's new reference build --
the largest single latency win on this project's whole latency-optimization
line, real board and real P&R verified.

What changed
------------
PW's weight read was re-reading the same weight data redundantly from DRAM on
every spatial (rt,colt) tile -- confirmed via real code+descriptor analysis
(ZHR-92, 2026-09-01) at up to 341.3x redundancy for shallow/wide-spatial
layers, 13.22x weighted average across the network's 26 real PW layers. A
real board test (address-fixed-but-wrong-values probe) measured this costing
55.3% of a representative layer's own time (entry3: 50.70ms -> 22.67ms) --
directly refuting a much older, architecturally-different-mechanism's 1.3%
result that had closed this line for weeks.

Source change (mac_array_raster_integrated.cpp):
  - New `pw_weight_cache[147456]` (144KB = exactly layer_0040_pwconv's real
    weight count, the largest of the 22 real PW layers this cutoff covers),
    declared in run_layer's own scope, NO array partition (PW_FLAT only ever
    reads 1 element/cycle, MAC_PD=1 -- a plain BRAM array already satisfies
    that, no complete/cyclic partitioning needed or used).
  - New `PW_WEIGHT_HOIST` loop, gated `op_type==PWCONV && pw_cacheable`,
    positioned exactly like the pre-existing `PW_BIAS_HOIST`/`PW_SHIFT_HOIST`
    (once per layer, before the `(rt,colt)` spatial sweep begins).
  - `pw_cacheable = (cin*cout <= 147456)`, computed once per layer. The 4
    real PW layers that exceed the cutoff (layer_0043/44/47/48_pwconv, all
    with weight >144KB) fall back to the pre-existing direct-DRAM-read path,
    completely unchanged -- these 4 layers also have the LOWEST redundancy
    (4.0x) of any real PW layer, so they had the least to gain anyway.
  - `PW_CACHED` is a RUNTIME bool, not a 2nd template dimension alongside the
    pre-existing `FAST_WRITEOUT` -- an earlier attempt at a 2nd template bool
    produced 4 independently-synthesized instantiations (+21.2% LUT, and
    pw_weight_cache itself got physically duplicated once per PW_CACHED=true
    caller, +128 BRAM_18K instead of the expected +64 -- see CLAUDE.md's
    "array parameter read from 2+ instantiations" entry). The runtime-bool
    form was checked against the specific mechanism that forced an earlier,
    unrelated template dispatch (FAST_WRITEOUT: two mutually-exclusive
    branches WRITING to the same shared AXI port, which the scheduler
    couldn't prove non-conflicting -- forcing II=2) -- PW_CACHED's two arms
    are a BRAM read (no AXI port at all) vs. an AXI READ on gmem_w, only one
    of which ever touches a shared port, so that specific II-regression
    mechanism doesn't apply. Confirmed via csynth: PW_FLAT holds II=1 in
    both FAST_WRITEOUT instances under runtime PW_CACHED.

Real P&R (no pblock, route_design alone, no phys_opt needed)
--------------------------------------------------------------
  WNS:  +0.153200ns             (was +0.272136ns -- still positive, closed)
  LUT:  31,953/53,200 (60.06%)  (was 31,520/53,200, 59.25%, +1.37%)
  BRAM: 74/140 tiles (52.86%)   (was 34/140, 24.29%, +40 tiles)
  DSP:  52/220 (23.64%)         (was 48/220, 21.82%, +4)

Isolated csynth had projected BRAM at 98/140 (70%) by extrapolating the
+128 BRAM_18K delta seen in the earlier (now-abandoned) template-dispatch
form -- real P&R came in meaningfully better (74/140), the first instance on
this whole "isolated vs real" investigation line where the isolated number
was more PESSIMISTIC than reality, not more optimistic (see CLAUDE.md).

Board verification (2026-09-02, this round)
---------------------------------------------
Single-op, byte-exact:
  - PW cached path (board_test_entry3, cin=cout=48, 64x64, well under the
    144KB cutoff): PASS, 0/196,608 mismatches, 50.70ms -> 22.66ms (-55.3%,
    matching the earlier address-fix probe's 22.67ms almost exactly, this
    time with genuinely correct values).
  - PW fallback path (board_test_entry64, layer_0043_pwconv, cin=384/
    cout=1152, 432KB weight, the largest real PW layer, >144KB so this
    dispatches through the unchanged direct-read path): PASS, 0/73,728
    mismatches, 120.10ms vs. the old baseline's own re-measured 120.68ms on
    the identical bundle -- no regression, confirming the fallback path is
    untouched.

Full network (82 entries, board_test_full_network bundle):
  - 82/82 entries written, no timeouts, no hangs.
  - PL-side total: 2,111.27ms vs. baseline's 3,630.74ms -- -1,519.47ms
    (-41.9%), the largest single latency win on this project's whole
    latency-optimization line to date.
  - Six-checkpoint correctness: see the note below on how this is reported
    -- verified against `ckpt_ref_*_0000.npy` (the ONNX float32 reference,
    dated 2026-08-21, independent of this or any prior HLS/C++ change),
    NOT against `ckpt_hw_*` (the csim fixed-point reference). Cosine
    similarity per checkpoint (stage1/stage2/stage3/stage4/finaldw/se):
    0.6313 / 0.1286 / 0.2227 / 0.3491 / -0.2459 / -0.2811 -- matches the
    project's own long-established figures for these six checkpoints
    exactly (4 decimal places), on both this build and a same-session
    re-measurement of the old baseline.

IMPORTANT correction on correctness methodology (read before citing
"byte-exact" for ANY board round after 2026-08-23)
---------------------------------------------------------------------
`ckpt_hw_*` (the csim fixed-point reference `tools/compare_board_full_
network_ckpts.py` also reports a mismatch count against) is CURRENTLY NOT
TRUSTWORTHY as a standalone correctness claim. Confirmed this round: it was
last legitimately generated ~2026-08-23 (`mac_array_ckpt_dump.cpp`/`run_
ckpt_dump.tcl` last touched that day, the same day the comparison script
itself was created against a since-superseded tile-based-DW mechanism), and
was never regenerated across three real deployed-architecture changes since
(PW_FLAT II fix 2026-08-27, DW raster integration 2026-08-28, gmem_meta
elimination 2026-08-31) -- meaning every prior "N/N checkpoints byte-exact"
claim in this project's history after 2026-08-23, including the immediately
prior deployed baseline's own README, was either comparing against this
same stale reference or (after 2026-08-31) couldn't have actually re-run
the check at all, since the interface change broke the tool's compile.
Fixed the tool's interface + which implementation file it links this round,
but a fresh regeneration STILL doesn't match real board output byte-for-
byte -- a second, independent, NOT YET ROOT-CAUSED bug, most likely inside
`mac_array_ckpt_dump.cpp`'s own 82-entry host-side orchestration (see
CLAUDE.md's "Known open issues" for the two ruled-out hypotheses --
wrong-implementation-linked, refuted; DW-mechanism-choice, refuted -- and
the not-yet-attempted next step, entry-by-entry bisection).

Going forward: report board correctness primarily via cosine similarity
against `ckpt_ref_*.npy`, not byte-exact count against `ckpt_hw_*`. The
byte-exact-vs-csim check remains useful for a narrower, same-round question
("did THIS change regress vs. the immediately prior round," which is how
this round used it to first prove weight-hoist wasn't the cause of the
mismatch) but is not, on its own, a standalone correctness claim until
`ckpt_hw_*`'s own generation is root-caused and re-verified fresh.

Golden rollback image /lib/firmware/fastvit_bd_wrapper.bin was NOT touched
(md5 7ee26f67a1fca38a2752e99cf0bac25b, verified before, during, and after
this round's deployment).

Source
------
fastvit_ip_v2/mac_array_raster_integrated.cpp (PW_WEIGHT_HOIST, pw_weight_
  cache, PW_CACHED runtime dispatch -- see the source's own header comments
  at the pw_weight_cache declaration and pw_flat_pipeline_impl's template
  declaration for the full design rationale)
fastvit_ip_v2/mac_array_ckpt_dump.cpp, fastvit_ip_v2/run_ckpt_dump.tcl
  (interface fix + correct-implementation link fix, see above)
fastvit_ip_v2/pw_weight_hoist_tb.cpp (new dedicated csim testbench -- neither
  pre-existing testbench covers PW's cached/fallback boundary; 6 cases
  spanning all 3 real-network-live template instantiations plus the
  believed-dead-in-real-network 4th, including the exact 144KB cutoff edge)
tools/build_single_op_test_entry64.py (new single-op bundle builder for the
  >144KB fallback layer, using freshly-dumped real-chain entry_63/64.bin)

HLS solution: fastvit_ip_v2/pw_weight_hoist1/s1 (csynth+export, 10ns/100MHz).
Vivado: vivado_impl/run_impl_bitstream_pw_weight_hoist.tcl (P&R + bitstream
in one script, route_design only, no phys_opt needed).

Files in this directory
------------------------
mac_array_bd_wrapper_pw_weight_hoist.bit          -- Vivado bitstream (not
                                                        byte-swapped)
mac_array_bd_wrapper_pw_weight_hoist_swapped.bin  -- byte-swapped .bin,
                                                        board-loadable
                                                        (pulled directly from
                                                        the board's own
                                                        /lib/firmware after
                                                        load via the board's
                                                        fpga_overlay.py
                                                        Overlay class, not
                                                        locally re-derived)

Deployed to board as /home/root/fpga/mac_array_bd_wrapper_pw_weight_hoist.bit.

Regenerate
----------
1. fastvit_ip_v2: vitis_hls -f run_export_ip_pw_weight_hoist.tcl (fresh
   project name each time, per this project's export_design stale-cache
   discipline)
2. vivado_impl: vivado -mode batch -source run_impl_bitstream_pw_weight_hoist.tcl
   -nolog -nojournal (P&R + bitstream in one script; writes .bit directly,
   no separate bitstream-only step needed since no phys_opt was required)
3. Board load: the board's own /home/root/fpga/fpga_overlay.py Overlay class
   does its own byte-swap + /lib/firmware deploy from the raw .bit -- no
   separate local byte-swap step needed.
4. ARM test driver: register map is UNCHANGED from mac_array_a3_gmemmeta_
   elim1 (PW_CACHED is entirely internal to the IP body, not a new register)
   -- the existing compiled /home/root/mac_array_single_op_test and
   /home/root/mac_array_full_network_test binaries can be reused as-is, no
   rebuild needed.
