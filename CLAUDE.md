# FastVIT HLS Accelerator — working conventions

MicroZed **xc7z020-1CLG400C** (confirmed 2026-08-13: standard MicroZed 7020 SOM only ships in this
speed grade). Board reachable at `root@192.168.1.50` (passwordless SSH, PYNQ-ish Linux). ARM
cross-compiler (`arm-linux-gnueabihf-gcc`) lives on the build server `patrick@192.168.1.87`. Vivado
2024.2 / Vitis HLS 2024.2 run locally on this Windows machine.

Full work history is tracked in **Linear**, team `ZHR`, project "FastVIT HLS Accelerator" — read it,
don't just rely on this file or local memory. Two issues are load-bearing context before touching
anything architecture- or timing-related:

- **ZHR-63** ("15. 架构复现主线重排") — the current mainline plan and its phase breakdown.
- **ZHR-64** ("16. ActiveSight 论文复现基准") — paper-vs-project comparison numbers, updated as real
  measurements come in (see the 2026-08-13 correction comment: real MAC utilization is ~15-17%, not
  the original ~1% estimate).

## The actual goal (redefined 2026-08-13, see ZHR-63/ZHR-64)

Reproduce the **ActiveSight (MobiCom'26)** PatchDW detector accelerator on this same xc7z020 chip —
**not** generic optimization of the deployed v18gelu bitstream's 1633.8ms. The paper hits
200MHz/73.68 GOPS/32k LUT on the same chip; this project uses almost the same LUT budget for a
fraction of the throughput. The gap is dominated by architecture (no unified MAC array — this
design dispatches to 5 separate fixed-function HLS workers via a shared op_code register), not
clock frequency. **200MHz is still a required reproduction target** — it was never actually
abandoned, only resequenced to Phase D, after the architecture is replaced. Do not cite ZHR-11/12/14
(the old architecture's 200MHz timing conclusions) to justify skipping 200MHz work on a new
architecture — those results are voided once the architecture changes.

Phase plan:

- **Phase 0** (2026-08-13, mostly done): git history/tagging, speed grade, per-layer timing + MAC
  utilization, accuracy harness, SE-block/FinalDW correctness investigation. No HLS changes, no P&R.
- **Phase A**: replace op_code-dispatch + 5-worker + shared-m_axi with a layer-controller + one
  time-multiplexed pr×pc×pd=8×8×8 MAC array.
- **Phase B**: migrate the `dsconv_worker` DW+PW on-chip fusion work (ZHR-16) into the new
  architecture. Use round-2's netlist as the migration starting point, not round-5's. Add serpentine
  scanning (paper uses it). Recompute DRAM traffic from scratch under the new architecture.
- **Phase C**: W8A4 quantization via CLIP contrastive-distillation training — naive PTQ already
  failed catastrophically (ZHR-13), this must be redone from the training side.
- **Phase D**: re-attempt 200MHz on the new architecture, timing conclusions evaluated fresh.

## Hardware constraint: Zynq HP port is not cache-coherent (confirmed 2026-08-14/15, ZHR-8 Phase 0.7)

The ARM core and the FPGA IP share DRAM buffers (ping/pong feature buffers, weights) over the
Zynq-7000 HP AXI port, which is **not cache-coherent**. Any time the ARM writes a buffer the IP will
read (e.g. the initial test-image `memcpy`), the ARM must `fv_cache_flush()` it first; any time the
ARM reads a buffer the IP just wrote (e.g. `se_block()` reading FinalDW's output), the ARM must
`fv_cache_invalidate()` it first. Skip either and the ARM silently reads/writes its own stale cache
line instead of what's actually in DRAM — the IP itself can be computing correctly the entire time
(confirmed for FinalDW, ZHR-8 step 6) while the ARM-visible result is wrong, and the wrongness looks
exactly like a normal, in-range numeric error, not a crash — nothing about it announces itself.

**This is a permanent architectural constraint, not an artifact of the current op_code-dispatch
architecture — Phase A does not fix it automatically.** A layer-controller + unified MAC array still
needs the ARM to feed the initial input and read the final output through the same non-coherent HP
port. Any new datapath design (Phase A onward) must budget explicit flush/invalidate at every
ARM↔DRAM handoff point from the start, not bolt it on after the fact.

Current fix (as of commit 2cd8374): pushed into `fastvit_driver.c`'s `fv_run_conv/dwconv/pwconv/
add/gelu` facade functions directly (flush inputs before dispatch, invalidate outputs after
`fv_wait_done()`), so every caller gets correct cache management automatically instead of relying on
scattered per-call-site fixes. See ZHR-8's Phase 0.7 steps 5-9 for the full root-cause chain,
including a 2-variable-confound false conclusion at step 5 that got corrected at step 6 — worth
reading before assuming a "cache-safe" test proves anything about the non-cache-safe path it's
supposedly standing in for.

## Hard stop list (do not do these unless a session explicitly says the phase has changed)

- No more 200MHz P&R on the **old** architecture (op_code+shared-m_axi) — exhausted across dozens of
  rounds (ZHR-11 Tier A x10, ZHR-12 Tier B x4 granularities, ZHR-16 x6 rounds).
- No more placement/phys_opt directive rotation as a timing lever — falsified 4 separate times. **A
  targeted `pblock` is not the same lever and is not covered by this ban** (confirmed working
  2026-08-22, ZHR-92 option E): the 4 falsified rounds were blind strategy-swapping (`place_design`/
  `phys_opt_design` directive rotation) with no specific physical target. A `pblock` constraining a
  known, diagnosed hierarchy to a compact region — used once, targeting a critical path whose own P&R
  record already showed "zero logic levels, high routing-delay share" (a placement-distance signature,
  not a fan-out or density signature) — took `route_design` from -0.345ns (needing `phys_opt` to limp
  to +0.004ns) to +0.165ns alone, no `phys_opt` needed, at zero LUT cost. The difference is a specific
  diagnosed target vs. blind rotation, not "pblocks are magic" — don't reach for one without a P&R
  record that actually shows a distance signature first. **A pblock is not a one-time constraint —
  its size must be re-evaluated after any change that materially shifts LUT usage, not set once and
  forgotten.** Confirmed costly twice, not once: a design that grew (a later DW whole-block-burst
  attempt) failed to *route* inside a pblock sized for the smaller pre-growth design (170+ unroutable
  pins); a design that later *shrank* (66.53%→55.07% LUT from further DW staging fixes) still failed
  to close timing (-0.003ns) inside that same unchanged pblock, because fewer cells in a fixed-size
  region let the placer spread them out more, lengthening exactly the routes the pblock exists to
  shorten — widening it again (X0-96→X0-120) fixed it on the first try both times, confirming this
  isn't a fluke. Rule: any round that materially changes LUT occupancy re-checks pblock sizing as
  part of that round, not as an afterthought once timing already fails. **Confirmed a third time,
  2026-08-23 (ZHR-92, accumulator-rewrite round): design shrank 59.09%→57.26% inside the unchanged
  X0-120 pblock, WNS went from -0.122ns to -0.451ns (route-delay share 40.4%→51.4%) — TIGHTENING
  to X0-96 (not widening) fixed it (WNS +0.112ns, route-delay share back to 38.3%).** This refines,
  not contradicts, the two rounds above: the direction (widen vs. tighten) isn't determined by
  grow-vs-shrink alone, it's determined by whether the pblock has become too tight (routing failure
  — widen) or too loose for the design's current footprint (cells spread out, routes lengthen —
  tighten). Self-violation caught the same day: the VERY NEXT round's pre-registration carried
  forward "pblock 保持 X0-96" by default from the round that had just shrunk, without checking that
  the next round's own code change (an outer-hoisted lookup table) GREW the design instead — P&R
  failed to route (83 unroutable pins) at X0-96. Check the actual LUT direction of THIS round's
  change against the pblock size already in place; never carry a prior round's pblock size forward
  by default just because it worked last time.
- No P&R for ZHR-16's PATCH_GROUP "scheme 3" — csim-clean is the finish line; the shape gap it covers
  doesn't occur in real FastVIT-T8.
- **DSP-packing and MAC_PD-expansion (channel-parallelism widening) are both CLOSED, not deferred —
  CORRECTED 2026-08-31.** The premise this bullet used to state ("old rejections assumed an LUT-bound
  chip") is wrong: re-evaluated both against the gmem_meta-elimination build's much wider real margin
  (LUT 59.25% vs the old 77.52%, WNS +0.272ns) specifically to test whether resource headroom was ever
  the real blocker. It wasn't, for either line. DSP packing's II 1->2 regression (`inline_check.log`)
  and a freshly-run MAC_PD=2 real P&R + board test (ZHR-92, ~same day) both trace to the **identical**
  root cause: `gmem_w`'s single AXI port can't service more than one bus request per pipeline
  iteration ("Unable to schedule bus request operation... due to limited memory ports (II = 1)" --
  the exact HLS diagnostic, both times). MAC_PD=2 routed clean and passed real P&R timing (WNS
  +0.222ns) and was byte-exact correct on real hardware -- and still made the real board 3.24%
  SLOWER (entry-level PW layers up to +12.6%), because `PW_FLAT`'s achieved II regressed 1->2 for
  the same port-contention reason, exactly canceling the halved iteration count. **Neither line is
  resource-bound (LUT/DSP/BRAM) -- both are bound by `gmem_w`'s single-port AXI bandwidth**, a
  completely different resource that headroom on the other three doesn't touch. Revisiting either
  again for the same reason (more headroom) would repeat this same negative result -- the actual
  unlock, if ever pursued, is widening/partitioning `gmem_w`'s own AXI interface (a second m_axi
  port, or a wider burst), not tuning MAC_PD, DSP binding, or waiting for more LUT/DSP room.
  **Refined same day, later round (2026-08-31): the verdict above ("closed") is right, but "II can't
  be fixed" would be the WRONG reason -- it can. A packed weight read (`ap_uint<8*MAC_PD>` over the
  same `gmem_w` bytes, exhaustively confirmed 100% 2-byte-aligned across all 26 real PW layers' full
  ot/cbase/step address ranges, 1,434,240 addresses checked) restores `PW_FLAT`'s achieved II to 1 at
  MAC_PD=2 -- but only once the fast/slow dispatch is a **template** bool
  (`pw_flat_pipeline_impl<FAST_WRITEOUT, FAST_LANE_READ>`, mirroring FAST_WRITEOUT's own precedent
  directly above), not a runtime `if/else`: a first attempt with a runtime branch left II stuck at 2,
  and the HLS diagnostic named the SLOW path's own leftover per-element reads (not the fast path) as
  the unschedulable operation -- confirming the mechanism is identical to FAST_WRITEOUT's own
  mutually-exclusive-branch problem, not a burst-inference miss. Real cost of the template fix,
  measured (not estimated) via real csynth of both real-network instantiations
  (`<true,true>`/`<false,true>`, both achieved II=1): isolated top-level LUT +18,396 (+33.2%) over the
  2-instance PD2 baseline, which -- using the SAME-DAY isolated-vs-real P&R ratio measured on this
  exact PD2 baseline (55,415 isolated / 35,118 real = 1.578x) -- projects to roughly 86-88% real LUT,
  leaving only ~12 points of margin on a line where 77% occupancy already made WNS marginal and 59%
  was needed for a comfortable +0.272ns. A second form (factoring the ~20-line gather into its own
  `template<bool FAST_LANE_READ>` helper with `#pragma HLS INLINE off`, hoping HLS/Vivado would share
  the ~130 lines of unchanged surrounding logic across the now-4 `pw_flat_pipeline_impl` instantiations
  instead of each carrying a full private copy) measured NO benefit -- LUT dropped only 73,811->72,287
  (-2.1%), and the csynth report itself shows why: each of the two real-network callers bound its own
  physically separate instance of the helper (`grp_read_pw_lane_weights_true_s_fu_1224` vs `_fu_1207`,
  different functional-unit indices, not a shared instance) -- see the new bullet below, this is the
  4th confirmed instance of that pattern, now confirmed to hold even for an explicitly-outlined
  (`INLINE off`) helper, not just inlined branches. **Correct framing going forward: "II is fixable,
  the fix's resource cost is not worth its ceiling" -- not "II can't be fixed."** This distinction
  matters because it's reversible: a future structural LUT reduction on the scale of the gmem_meta
  elimination that produced this round's headroom in the first place would be grounds to re-open this
  specific fix (the template-specialized packed read), without re-deriving the alignment/II work above
  -- "unfixable" would wrongly foreclose that.
- **STALE NUMBER OVERRIDE, 2026-09-01 (ZHR-92, real 4-way LUT decomposition round): a "compute is only
  ~16.4% of LUT (4,256 LUT), staging/glue is 3x that" figure has been circulating and cited from memory
  in this project -- it is from BEFORE the DW-raster integration and gmem_meta elimination, describes
  an architecture that no longer exists, and does not apply to the current deployed baseline. Do not
  cite it again.** Fresh real post-synthesis hierarchical decomposition
  (`vivado_impl/gmemmeta_elim1_impl/hier_util_baseline.rpt`, `report_utilization -hierarchical` on the
  deployed baseline's own `.dcp`) of the CURRENT `mac_array_a3_gmemmeta_elim1` build (28,788 LUT/48 DSP
  at this report's own counting basis -- ~9% below the officially-documented 31,520, an unresolved
  counting-methodology gap flagged, not resolved, elsewhere in this file): **compute (PW's
  `Pipeline_PW_FLAT` fused gather+MAC+writeout, both instances, 4,025 LUT + DW's `CROW_CCOL` fused
  MAC+clip+writeout, 8,570 LUT) is 12,595 LUT -- 43.8% of the total, not 16.4%.** Staging is only
  ~10.5% (~3,017 LUT: DW's line-buffer `dwr_produce2`, `ROW_READ_FILL`, `COPY_FROM_ROW`, PW's small
  bias/shift caches). Control/glue is ~31.7% (run_layer's own glue, fpg-dispatch, the 6 elementwise
  ops, address arithmetic, plus ~2,469 LUT of unitemized small primitives folded in here). AXI adapters
  are 14.0% (4,044 LUT: control_s_axi + all three m_axi masters). **The single most load-bearing finding
  from this same round: of the whole design's 48 used DSPs, ZERO are in the PW or DW MAC arrays --
  every DSP is address arithmetic (33 in DW's own index/offset multiplies + fpg-dispatch, 14 in
  run_layer's top-level address math, 1 in GELU). Both `Pipeline_PW_FLAT` and `CROW_CCOL` show 0 DSP in
  their own subtree -- the entire MAC datapath (both PW and DW) is 100% LUT-inferred, at 21.8% DSP
  utilization (48/220, 172 idle).** Caveat carried forward: both fused regions have no further
  post-synthesis sub-hierarchy (HLS didn't emit named sub-instances for gather vs. multiply vs.
  writeout within either pipeline), so 12,595 LUT is "the whole fused block," not a pure multiplier
  count -- true isolable multiplier primitives (named `mul_*` instances) total only ~450 LUT/27 DSP
  project-wide, and all of those are address arithmetic, not the MAC array itself (which has no
  separately-instantiated primitive at all -- it's inlined into the flattened RTL).
  **Cross-checked against FiLM-QNN's own real deployed resource split (Table 5: 41.3k LUT/78%, 208
  DSP/95%, PYNQ-Z2), using the project's own established per-unit LUT cost (33 LUT per 4-bit-weight x
  5-bit-activation LUT-based multiplier) and prior background on their packing split (1008 total
  multiply units, 880 DSP-packed via G-way packing, 128 LUT-based): their LUT-fabric multiply cost is
  only 128 x 33 = 4,224 LUT, ~10.2% of their 41.3k -- i.e. FiLM-QNN is not "84% staging" either; they've
  moved 87% of their multiply units off LUT and onto DSP (95% DSP utilization) specifically to free LUT
  for wider Tm x Tn parallelism. (Table 4's own "Op" LUT/DSP absolute columns, e.g. W4A5 Op DSP=1152,
  do NOT reconcile with PYNQ-Z2's real 220-DSP budget -- 1152+144=1,296 DSP needed vs 220 available, a
  5.9x mismatch -- so those columns were NOT used directly; they're some normalized/theoretical-only
  quantity per the "Table 4 is calculated peak, not real allocation" finding already on record, and
  Table 4's absolute Op numbers should not be cited as physical resource counts without first
  re-checking the paper's own caption/units.) **The reframed 63x-peak-gap hypothesis going forward:**
  the gap may not be primarily "the chip can't fit more MACs" -- it may be "this design has never built
  a real parallel MAC array bound to DSP the way FiLM-QNN does; the entire datapath (PW and DW alike)
  is one small, time-multiplexed, LUT-inferred pipeline with DSP sitting 78% idle." This reframing is
  the reason the DSP-packing/MAC_PD-expansion CLOSED verdict two bullets above does NOT automatically
  extend to it: that closure was specifically about the COMBINATION of DSP packing with MAC_PD
  channel-widening (new gmem_w bandwidth demand from more parallel lanes) -- a more conservative variant
  (same parallelism, same datapath, only rebinding the EXISTING multiply from LUT to DSP, zero new
  bandwidth demand) was never isolated and tested on its own. **Important prior-result flag for
  whoever runs that test next: DW's own multiply already has a build flag for exactly this
  (`LB_FORCE_DSP`, `dw_raster_layer.cpp:242,372`), and it was already tested via REAL whole-IP P&R on
  2026-08-28 (see the "HLS-level resource-binding choice" bullet above) -- forced-DSP gave essentially
  IDENTICAL real resource counts (LUT 41,222 vs 41,243, DSP 92 vs 92, not a release) and REGRESSED
  timing (WNS -0.108ns vs +0.021ns). Any fresh isolated-csynth number for DW's BIND_OP=DSP conversion
  should be read against that already-known real-P&R result, not treated as new information on its own
  -- per this file's own "check history before measuring" and "isolated csynth doesn't predict real
  synthesis" rules. The genuinely untested half is PW's `Pipeline_PW_FLAT` multiply, which has never
  had a BIND_OP=DSP variant built or measured, isolated or real.**
  **CLOSED, 2026-09-01, same round: this specific lever (rebind the EXISTING multiply from LUT to DSP,
  no parallelism change) does NOT release enough LUT to matter, even under the most optimistic reading.**
  Added `PW_FORCE_DSP` (`mac_array_raster_integrated.cpp`, mirroring `LB_FORCE_DSP`'s pattern exactly)
  and ran 4 same-day, same-testbench, apples-to-apples whole-IP isolated csynth builds (`set_top
  mac_array_top`, 10ns/100MHz): baseline 48,401 LUT/48 DSP; `PW_FORCE_DSP` only 47,029 LUT/80 DSP
  (-1,372 LUT, +32 DSP); `LB_FORCE_DSP` only 44,383 LUT/146 DSP (-4,018 LUT, +98 DSP); both 43,011
  LUT/178 DSP (-5,390 LUT, +130 DSP, exactly additive -- no interaction effect). All 4 acceptance
  criteria passed: II held at 1 for `PW_FLAT` (no regression); `CROW_CCOL`'s achieved II was
  unexpectedly already 8 (not the "II=2" this file's DW section cites elsewhere -- discrepancy noted,
  NOT resolved this round, flagged for whoever touches DW timing next) but did not regress further
  under `LB_FORCE_DSP`; csim (correct `mac_array_raster_integrated_wiring_tb.cpp` pairing, NOT
  `mac_array_tb.cpp` which hits the already-documented stale-pairing `DW_CIN_MIN_SAFE` assertion
  unrelated to this change) came back 4/4 PASS, 0 mismatches with both flags on; DSP headroom is fine
  (178/220, 80.9%). **But the number itself kills it**: even the best case (both forced, isolated
  basis, zero real-P&R discount applied) only closes 5,390 of the 12,434 LUT gap PW tiling needs
  (43%) -- and DW's own identical lever already has a REAL P&R precedent (2026-08-28, cited two
  paragraphs above) showing its real release is ~0 (41,222 vs 41,243 LUT), meaning the isolated
  4,018 LUT DW figure this round measured is very likely to mostly evaporate in real synthesis too --
  an 8th-ish independent confirmation of "isolated csynth doesn't predict real synthesis," this time
  specifically for a resource-binding-pragma change. No real P&R was run for `PW_FORCE_DSP` given this
  -- the arithmetic doesn't support it being sufficient even before spending that round. **Root cause
  of why this lever is too small**: PW's own parallelism is only MAC_PD(1)xMAC_PR(4)xMAC_PC(4)=16
  lanes, and DW's 98 DSP figure is `DWR_MAX_FPG(2) x DWR_MAX_K(7) x DWR_MAX_K(7)`=98 fully-unrolled
  taps, not a wide channel-parallel array -- rebinding a genuinely small MAC array from LUT to DSP
  only ever frees a genuinely small amount of LUT. The reframing from the round above (this design has
  never built a real DSP-bound PARALLEL MAC array) still stands, but closing it for real would need
  wider parallelism AND DSP binding together -- which is exactly the already-CLOSED
  DSP-packing+MAC_PD-expansion combination (gmem_w bandwidth wall), a dead end this file already
  documents. No new avenue opened by this round; both `PW_FORCE_DSP` and `LB_FORCE_DSP` stay
  undefined by default.
- 125/142.86MHz frequency midpoints: a cheap side-check during Phase A at most, never a mainline goal.

## Working method (non-negotiable, not stylistic)

- **Before citing a reference measurement as a per-unit calibration (ns/MAC, ms/byte, etc.), confirm
  what the formula for that reference's own "unit count" actually computes -- especially whether it
  silently assumes a different op shape (e.g. depthwise vs. regular convolution) than the real
  layer.** Confirmed 2026-09-01 (ZHR-92, DW-to-ARM migration costing round): Stem's real ARM time
  (381ms, an existing measurement) was about to be calibrated against `cin*k*k*h_out*w_out =
  442,368`, the MAC-count formula for a DEPTHWISE conv -- but Stem is layer 0, a REGULAR conv
  (`op_type=conv`, `cout=48`, `group=1`), whose real MAC count needs the `*cout` factor:
  `cin*cout*k*k*h_out*w_out = 21,233,664`, exactly 48x larger (cout=48). Using the wrong formula
  would have produced an ARM-per-MAC calibration (0.86 μs/MAC) 48x too slow, silently making every
  ARM-migration cost estimate built on it 48x too pessimistic (or, used in reverse, making any
  ARM-offload proposal look 48x cheaper than it really is depending on which direction the error
  compounds) -- corrected value: 17.94 ns/MAC. The general lesson: a "MACs = cin*k*k*h*w" shaped
  formula is only correct for depthwise-style convolution (one input channel per output channel);
  applying it to a regular/pointwise conv without the output-channel factor is a silent, large
  (proportional to Cout) undercount that doesn't announce itself the way a dimension-mismatch crash
  would -- verify which op type a reference measurement's own layer actually is before reusing its
  formula elsewhere.
- **A template specialization's LUT cost scales with how much code the template parameter wraps,
  not a fixed percentage learned from one prior instance.** Confirmed 2026-08-31/09-01 (ZHR-92, PW
  tiling proposal Step 2): the deployed `pw_flat_pipeline_impl`'s `template<bool FAST_WRITEOUT>` costs
  +12.6% LUT (real P&R, `32a6f1d`) for duplicating its whole body across 2 instances -- cheap because
  the wrapped gather logic is a handful of per-element reads. `pw_flat_tile_v2_impl` copied the exact
  same template shape (also wrapping its entire MTILE/CBASE/LOAD/COMPUTE/WRITEOUT body) onto a much
  heavier mechanism (32-wide burst-tile weight DMA) and the resulting whole-IP integration failed P&R
  outright at DRC -- 92,907 LUT required vs 53,200 available (175%), the largest "isolated csynth
  doesn't predict real synthesis" miss on this project by far (isolated had shown only 86%). Fix:
  narrowed the template to wrap ONLY the writeout section (small, matching the deployed mechanism's
  own actual proportions) with LOAD/COMPUTE factored into a single non-templated function with exactly
  one call site -- isolated LUT dropped 46,259->34,114 (-26%), confirmed via instance count that the
  compute logic really is synthesized once, not twice, though this landed above the ~25,000-28,000
  naive target for a different, specifically-identified reason (see the entry directly below on
  writeout's own real size), not because the sharing fix itself failed. **Before accepting a
  `template<bool>` (or any template) around a mechanism, estimate the wrapped code's own weight first
  and treat a prior round's "+X% was acceptable" specifically as evidence about that lighter mechanism,
  not as a transferable budget for a heavier one.**
- **Fully `complete`-partitioning both dimensions of an array with complex
  conditional per-element writes can make `csynth_design` itself crash
  with an out-of-memory exception, not just report a resource-budget
  problem -- and the failure mode is "runs for a long time, then dies,"
  not a fast/legible error.** Confirmed 2026-08-31 (ZHR-92, PW tiling
  proposal Step 2 lead-in): `pw_flat_tile_v2.cpp`'s `w_tile[32][32]`
  weight-tile buffer had `#pragma HLS ARRAY_PARTITION ... complete dim=2`
  only (Tn), achieving II=2 on its own fill loop (`LOAD_WORD`, a known,
  accepted, separate inefficiency -- see the entry below). Adding
  `complete dim=1` too (fully partitioning BOTH dimensions, the naive
  "just partition everything" fix for that II=2) did not produce a worse
  resource number to react to -- `csynth_design` ran for **18+ minutes**,
  peaked at **39.7GB** of allocated memory, and then failed outright:
  `ERROR: [HLS 200-103] Unexpected exception occurred: Out of Memory`,
  during `SCHED 204-61] Pipelining loop 'LOAD_WORD'`. The likely mechanism
  (not independently confirmed further, not worth the cost of doing so):
  full dual-dimension partitioning of a 32x32 array combined with this
  loop's own per-lane conditional writes (`byte_valid` gating each of 4
  unrolled write lanes per cycle) blew up the scheduler's combinatorial
  search space rather than merely producing a large-but-finite netlist.
  **The practical danger is the failure shape itself**: a normal
  over-budget resource result is legible and fast (seconds to a couple
  minutes, a number you can read and reject); this instead consumes a
  full round's worth of wall-clock time (and, on a resource-constrained
  machine, could exhaust real system memory) before producing any
  signal at all. Before applying `complete` partitioning to BOTH
  dimensions of an on-chip array that has complex conditional writes
  (not a simple unconditional fill), consider partitioning only the
  dimension actually contended for parallel access first, and treat a
  "why is this taking so long" csynth run past a couple of minutes as a
  signal to kill it and reconsider the pragma, not to just keep waiting.
- **An `ARRAY_PARTITION` pragma on a function PARAMETER does not carry
  through from the caller's own declaration site when that function is
  synthesized/tested as its own standalone top-level function -- it must
  be re-declared on the callee's own copy of the parameter, and omitting
  it is a real, tool-confirmed II regression, not redundant boilerplate.**
  Confirmed 2026-08-31 (ZHR-92, same round): `pw_patch_full` is declared
  with `#pragma HLS ARRAY_PARTITION variable=pw_patch_full cyclic
  factor=MAC_PD dim=1 / complete dim=2 / complete dim=3` at TWO places in
  the deployed codebase -- once where `run_layer` allocates it, and AGAIN,
  identically, inside `pw_flat_pipeline_impl`'s own body, on its own copy
  of the same parameter. This looked like harmless duplication until
  `pw_flat_tile_v2.cpp` (a new standalone top-level function taking
  `pw_patch_full` as a parameter, for isolated csynth testing) omitted
  the second declaration and reproduced a real II=8 violation on
  `pw_patch_full` (`HLS 200-885`, "due to limited memory ports") -- gone
  entirely once the identical pragma was re-added inside the new
  function's own body. The deployed code's apparent redundancy was not
  redundant; it was the fix for exactly this case, already applied
  pre-emptively. Any new top-level (or independently-synthesized) function
  receiving an array parameter that the ORIGINAL caller partitions needs
  the same partition pragma re-stated on its own parameter, not assumed
  inherited.
- **HLS does not share RTL across separate call sites/instantiations, even when the differing logic
  is deliberately factored into its own small helper with `#pragma HLS INLINE off`.** 4th confirmed
  instance of this pattern (after 3 earlier ones this project ran into during round 9/10/13, where two
  callers inside a `PIPELINE` region each got bound their own private copy instead of sharing one).
  Confirmed again 2026-08-31 (ZHR-92, PW_FLAT gmem_w port-contention fix, "form (b)"): extracting a PW
  weight-gather's fast/slow read logic into `template<bool FAST_LANE_READ> read_pw_lane_weights(...)`
  marked `#pragma HLS INLINE off`, then calling it from both of `pw_flat_pipeline_impl`'s two
  real-network instantiations (`<true,true>` and `<false,true>`) with the SAME `FAST_LANE_READ=true`
  template argument, did not produce one shared physical instance -- the csynth report names two
  separate functional units, `grp_read_pw_lane_weights_true_s_fu_1224` in one caller and
  `grp_read_pw_lane_weights_true_s_fu_1207` in the other, different fu indices, confirmed via the
  report text itself, not inferred from a resource total. Isolated top-level LUT dropped only 2.1%
  (73,811->72,287) versus the fully-inlined baseline -- nowhere near the "few hundred LUT" that would
  indicate real sharing. The intuition "factor the differing 20 lines into their own function, let the
  surrounding ~130 shared lines stay unduplicated" does not hold in HLS, at least not for callers
  inside a `PIPELINE`-region caller context, and `INLINE off` alone is not sufficient to force sharing
  across genuinely separate top-level function instantiations -- each caller's own binding pass treats
  the callee as its own resource to allocate, regardless of whether an identically-parameterized sibling
  instantiation already has one. Do not propose "extract the shared part into a helper" as a resource-
  saving move for two-or-more `PIPELINE`-region callers without a real csynth measurement backing it;
  the default expectation, based on 4 independent instances now, is that it won't share.
  **Refined 2026-09-01 (ZHR-92, PW tiling proposal Step 2): the operative condition is "two-or-more
  callers," not "extracted into a helper" per se -- when a mechanism is factored so there is only ONE
  caller (the shared code lives in a single non-templated function called from a single, non-duplicated
  site), sharing works exactly as hoped.** `pw_flat_tile_v2_compute_group` (LOAD+COMPUTE, the heavy
  32-wide burst-tile-DMA machinery) was factored out of a `template<bool FAST_WRITEOUT>` function into
  its own non-templated function, called from exactly one site in the top-level `pw_flat_tile_v2`'s own
  `MTILE` loop (itself not duplicated by any template) -- confirmed via instance count (a single
  `pw_flat_tile_v2_compute_group_csynth.rpt`, no `_true`/`_false` variants) that it synthesizes once,
  not twice, and isolated LUT dropped 46,259->34,114 accordingly. This is the first successful instance
  of "extract into a helper to share resources" on this project's own line of repeated failures -- the
  distinguishing factor from all 4 prior failures (round 9/10/13, and this same line's own "form (b)")
  is not the extraction itself, it's that those 4 cases all had TWO callers that were each already
  independently-duplicated (e.g. two template instantiations of an outer function each calling the same
  helper) -- HLS binds a private copy per caller in that shape regardless of `INLINE off`. A single
  caller has nothing to duplicate against. Before extracting a helper for sharing, count the real number
  of call sites first, not just whether the code is textually shared.
  **RULE EXTENDED to a new surface, 2026-09-02 (ZHR-92, PW weight-residency round): the same "2+
  independently-duplicated callers don't share" rule applies to a PASSED ARRAY PARAMETER, not just a
  called sub-function -- the first confirmed instance of this on that surface, after 4 confirmed
  instances on the "called helper" surface above.** `pw_weight_cache` (a 147,456-element, 144KB static
  array) is declared exactly ONCE, in `run_layer`'s own scope, and passed BY POINTER into
  `pw_flat_pipeline_impl<FAST_WRITEOUT,PW_CACHED>` -- a single logical array, one declaration site, one
  intent. Adding `PW_CACHED` as a second template bool (alongside the already-existing
  `FAST_WRITEOUT`) produced 4 independently-synthesized instantiations, 2 of which (`<true,true>` and
  `<false,true>`) both read `pw_weight_cache` as their own parameter. Real isolated csynth: BRAM_18K
  delta was +128, not the +64 a single 144KB array should cost (147,456 bytes / 18,432 bits-per-
  RAMB18E1 = 64.0 exactly) -- confirmed via `run_layer`'s own "Memory" utilization row (181 BRAM_18K)
  that the array is being counted/allocated as if TWO physical copies exist, one per PW_CACHED=true
  caller, not one shared BRAM shared across both. **Passing the same array into 2+ independently-
  instantiated template functions does not give you 2+ ports into 1 memory -- it gives you 2+
  memories, each looking logically identical but physically separate, each consuming its own real BRAM
  budget.** This roughly doubled the array's effective footprint (144KB logical -> ~288KB physical) on
  top of the already-known LUT duplication cost of the 2 extra function-body instantiations (+10,246
  LUT total, +21.2%, 4 instances not shared, matching the "called helper" version of this rule exactly
  for the surrounding compute logic). The fix under evaluation (not yet built) is the same one this
  rule's own "single caller" refinement above already prescribes: get PW_CACHED down to ONE caller
  (either eliminate it as a template dimension entirely via a runtime branch or a unified always-cached
  code path, or narrow what it wraps down to the single read line) rather than accept 2+ callers each
  reading the same logical array as their own private parameter.
  **CORRECTED same day, after actually building the fix: converting PW_CACHED from a template bool to
  a plain runtime bool fixed the LUT duplication (58,647->49,126, back to +1.5% over baseline, matching
  the "2 instances not 4" prediction almost exactly) but did NOT fix the BRAM duplication (BRAM_18K
  stayed at 195, unchanged) -- the attribution above ("get PW_CACHED down to one caller") was
  incomplete, not wrong in kind but wrong about which template dimension was the actual cause.**
  `pw_weight_cache` is still read from 2 separate call sites after this fix -- not 2 PW_CACHED=true
  instances anymore, but the SAME 2 FAST_WRITEOUT instances (`pw_flat_pipeline_impl<true>` and
  `<false>`) that were already there before this whole weight-residency round started, each now
  containing its own runtime `if(pw_cached)` branch and therefore its own reference to the array
  parameter. **The real rule is not "PW_CACHED's own templating causes the duplication" -- it's "ANY
  array parameter read from 2+ independently-synthesized instantiations of its ENCLOSING function gets
  a physical copy per instantiation, regardless of which specific template dimension (or none at all,
  if the read is unconditional) is responsible for that function having 2+ instances in the first
  place."** Removing PW_CACHED's own template dimension does nothing to fix this as long as
  `pw_flat_pipeline_impl` is templated on ANYTHING ELSE (here, `FAST_WRITEOUT`, which is load-bearing
  for an unrelated II=2 fix and not a candidate for removal) and both of those instances need the
  array. This generalizes the lesson further than the entry above states: before concluding a
  duplication problem is fixed by de-templating ONE dimension, check whether the SAME function still
  has other template dimensions with 2+ instances that also touch the resource in question -- a
  partial de-templating can fully fix the dimension you targeted while leaving the actual physical
  resource duplication completely unchanged, for a reason that has nothing to do with the change you
  just made.
  **REAL P&R RESOLVED THIS, 2026-09-02, same round: the isolated-csynth-based BRAM projection
  (98/140 tiles, 70%, extrapolated from the +128 BRAM_18K delta) was itself wrong -- real P&R came
  back at 74/140 (52.86%), a comfortable number, not a tight one.** This is yet another (an 8th-ish)
  confirmed instance of "isolated csynth doesn't predict real synthesis," but for the first time on
  this specific line the error is in the FAVORABLE direction (real came in much better than isolated
  projected, not worse) -- don't assume isolated-to-real divergence always means "the real number is
  worse," it can go either way, matching this file's own "Sixth confirmed instance" entry elsewhere
  which already made this same point about direction not being consistent. LUT (31,953/60.06%, vs.
  deployed baseline's 31,520/59.25%, delta +433/+1.37%) and DSP (52/220, 23.64%) both landed close to
  baseline and matched the isolated-csynth prediction closely -- it was specifically the BRAM number
  that isolated csynth got wrong here, not LUT/DSP, underscoring that even within one build, different
  resource categories can diverge differently; don't generalize "isolated was close on LUT" into "so
  BRAM will be close too." Timing closed clean (WNS=+0.153200ns, route_design alone, no phys_opt
  needed) -- this specific form (PW_CACHED as a runtime bool, FAST_WRITEOUT the only remaining
  template dimension, 2 instances) is a real, P&R-verified, deployable-quality result on the weight-
  residency line, not just an isolated-csynth number to distrust.
- **One round = one hypothesis + one measurement + one conclusion.** Report the result and stop —
  do not chain straight into the next round without a human checkpoint. This is not a suggestion:
  ZHR-16's round 3→4→5→6 ran back-to-back with no checkpoint and the user identified that as the
  reason the project stalled for two weeks. If a debugging chase runs past 2-3 rounds without a
  natural conclusion, stop and check in even if each individual round felt justified in the moment.
- If a "fix one bottleneck, the next equally-bad one pops up" pattern appears, STOP and name it as an
  architectural pattern — do not keep fixing individual instances.
- **When a tool's behavior itself is the obstacle (not the design), check official docs/example repos
  for a purpose-built mechanism before spending more rounds trying to make the tool's default
  heuristic accept the code.** Confirmed 2026-08-24 (ZHR-92): four rounds tried to get Vitis HLS's
  burst *inference* to accept WRITEOUT/PW_PATCH_HOIST/gmem_w's access patterns (conditional-branch
  restructuring attempts, `memcpy`, `config_interface` widen/alignment/latency/burst-length knobs —
  all failed or made things worse, one pushed the design 55% over the LUT budget) before checking
  whether Xilinx ships an explicit alternative. It does: `hls::burst_maxi` (`Interface/Memory/
  manual_burst` in `Xilinx/Vitis-HLS-Introductory-Examples`) issues a burst by direct API call
  (`read_request`/`read`/`write_request`/`write`) instead of pattern-matching a loop — it doesn't run
  the inference analyzer at all, so `AccessInCondBranchMissed` and its siblings simply don't apply.
  Verified synthesizable under this project's actual `-flow_target vivado` IP-export flow (the
  official example uses `-flow_target vitis`, not the same flow — confirmed separately, not assumed
  transferable), clean csynth ("All loop constraints were satisfied" — the first time any burst-related
  csynth run in this project has said that), tiny resource cost at probe scale (2,535 LUT). The general
  lesson: three-plus failed rounds fighting a heuristic is itself a signal to look for a manual/explicit
  escape hatch in the vendor's own toolkit before continuing to negotiate with the heuristic.
- **Before starting a new round of real measurement, check whether this project already measured it.**
  Confirmed valuable 2026-08-25 (ZHR-92): a DW per-tile cost round was about to build and board-test a
  fresh probe from scratch; checking git history first found half the decomposition (the per-ot
  component, 1,187.5 cycles/ot) was already measured on real hardware in an earlier round (`c8400b1`).
  Commit ordering (`c8400b1` predates the DW flat-pipeline attempt/revert) plus an empty `git diff`
  against the current source confirmed the old number was still valid against today's code — no
  re-measurement needed, only the missing other half (per-tile) had to be measured fresh. This project
  has run dozens of rounds; a lot of specific numbers already exist in commit messages and Linear
  comments. Check before spending a board round re-deriving something already on record — search git
  log/commit messages for the quantity or shape you're about to measure before writing a new probe.
- **PW re-reads every weight from DRAM on every spatial tile, confirmed 2026-09-01 (ZHR-92) via real
  code + real descriptors, not estimated -- and a mechanism that fixed exactly this was already built,
  measured, and reverted once, with a historical real-board result that should be treated as the
  starting prior for any future attempt, not ignored.** `pw_flat_pipeline_impl` is called once per
  `(rt,colt)` spatial tile (`run_layer`'s own comment confirms: "Called once per (rt,colt) tile...
  since the flat pipeline handles every ot/cbase internally") and re-reads all of that layer's weights
  from `w_base` inside every call (`PW_FLAT_STEPS_PER_CBASE = MAX_CIN_PW/MAC_PD = 32` gather steps per
  `(ot,cbase)`, one weight read each). For the real network's 26 PW layers this gives a weight-read
  redundancy ranging 341.3x (shallow, spatially-wide layers, e.g. layer_idx=2: 48x48, 64x64 spatial,
  256 tiles) down to 1.0x (SE block, h=w=1, no spatial reuse) -- weighted overall 13.22x (37,933,056
  bytes actually streamed vs. 2,868,480 bytes of unique weight across all 26 layers). **History check
  (this is a REGRESSION, not a new discovery): `38e7001` (2026-08-22) built and then reverted exactly
  this fix (`pw_weight_cache`, a 442KB static on-chip cache sized for the largest single layer's real
  weight footprint, eliminating the same per-tile re-read in the PRE-PW_FLAT-rewrite architecture's
  `run_reduce_unified`/`PW_WSTAGE` mechanism). Real board result: 700.27ms -> 690.37ms, only 1.3%
  faster, while costing 89-95% of BRAM and being the #2 worst P&R timing path at the time (that
  round's own margin was only +0.047ns, near-zero) -- judged a bad trade and reverted. Two caveats on
  reusing this number: (1) it's the ONLY real-board data point that exists for this exact redundancy
  pattern -- if it holds today, it implies PW's wall-clock time is not actually DRAM-weight-read-bound
  (the AXI reads are likely overlapped/hidden behind the II=1 compute pipeline's own cycle count, which
  is the real bottleneck instead), meaning eliminating the 13.22x DRAM traffic would do almost nothing
  for real time despite looking like a huge redundancy on paper; (2) it was measured on an EARLIER PW
  mechanism (op_type-dispatch `run_reduce_unified`, before the `PW_FLAT` single-flattened-pipeline
  rewrite that followed across 2026-08-23~27) under a MUCH tighter timing margin than today's baseline
  (+0.047ns then vs. +0.272ns now) and a tighter BRAM budget than today's (34/140 tiles used now,
  24.29%, vs. whatever was already occupied then) -- so it is not a certain predictor for a hoist
  re-attempted on current `PW_FLAT`, only the most relevant prior evidence available, and the 1.3%
  result must be addressed directly (not assumed superseded by more headroom) before re-opening this
  line. **On-chip residency facts, confirmed the same round via real descriptors +
  `gmemmeta_elim1_impl/utilization_gmemmeta_elim1.rpt`** (real routed BRAM: 34/140 tiles used, 24.29%,
  477KB currently free of 630KB total device capacity, both numbers real not estimated): across all 51
  real dispatched conv-family layers (26 PW + 25 DW), the single largest layer's own weight footprint
  is 442,368 bytes (~432KB, layers 43/44) -- under the 477KB free budget on its own. **Zero of the 51
  layers have a weight footprint too large to fit in currently-free BRAM; activations (input+output),
  not weight, are always the reason a layer can't be fully on-chip-resident** (44/51 layers fit
  weight+input+output entirely on-chip today; the 7 that don't -- layers 1,5,6,9,10,43,44 -- all fail
  because of activation size, not weight size). This makes "keep this layer's weights on-chip,
  stream only activations" geometrically possible for every real layer in the network, matching the
  historical `pw_weight_cache`'s own sizing choice (442KB, sized for the worst-case layer) almost
  exactly.
  **DECISIVE FOLLOW-UP, 2026-09-02, real board test -- the 1.3% historical caution above does NOT
  apply to the current PW_FLAT architecture; this line is REOPENED.** Built 3 `#ifdef`-guarded
  timing-only probes (`PW_FIX_WADDR`/`PW_FIX_ACTADDR`/`PW_FIX_OUTADDR`, `mac_array_raster_
  integrated.cpp`, all off by default), each forcing exactly one of PW_FLAT's three DRAM-adjacent
  addresses (weight read, `COPY_FROM_ROW`'s activation read -- NOT the dead `PW_PATCH_HOIST` the
  round's own request named, confirmed deleted, see the file's own "PW_PATCH_HOIST is now dead code"
  comment -- and the output write) to a single fixed, in-range address, leaving op count/structure
  untouched -- values wrong by design, timing real. Real board (`board_test_entry3`, cin=cout=48,
  64x64), baseline 50.70ms: weight-fixed **22.67ms (-55.3%)**; activation-fixed 50.73ms (~0%,
  within noise); output-fixed 50.69ms (~0%, within noise). **Weight-read address is the dominant
  driver, activation and output writes contribute nothing measurable.** Using the pre-registered
  total/internal=5.24-6.02x split, weight explains ~67% of PW's external cost; ~27% of total time
  (~13.7ms) remains unexplained by any of the three (candidates: AXI-Lite descriptor/control
  handshake, `PW_BIAS_HOIST`/`PW_SHIFT_HOIST` setup, `ROW_READ_FILL`'s own per-row DRAM read --
  untested this round -- or fixed per-iteration pipeline latency even at zero address cost) -- not
  fully closed, a real remainder to chase next, not "nothing else exists." **Reliability caveat on
  the headline number**: `PW_FIX_WADDR`'s own real P&R did NOT close timing (route_design
  WNS=-0.944ns; `phys_opt_design`, single-threaded per this file's own two-phase recipe, improved it
  to -0.723ns, still negative) -- the 22.67ms figure is NOT from a formally timing-closed build.
  Cross-checked instead of assumed: the route-only and phys-opt'd builds are two measurably different
  physical implementations (different WNS, different routing), each board-tested 3x (6 runs total) --
  every run landed at 22.63-22.69ms with the IDENTICAL mismatch count (164,054/196,608) bit-for-bit,
  every time. Two physically different timing-violating implementations producing bit-identical wrong
  output and matching timing is the signature of deterministic RTL behavior, not a physical
  metastability artifact (which would be expected to shift with routing) -- treated as real,
  reproducible evidence, with slightly lower confidence than the other two (cleanly WNS-positive)
  builds, not as certain as a closed-timing result would be. **Practical conclusion: the old
  `pw_weight_cache`'s 1.3% real-board result was measured on the PRE-PW_FLAT architecture
  (`run_reduce_unified`) and does not transfer -- PW_FLAT's weight-read cost is roughly 40x larger in
  relative terms (55.3% vs 1.3%) on the same redundancy pattern. Root cause of the gap between the two
  architectures not yet investigated. A real weight-residency mechanism for the CURRENT PW_FLAT
  architecture (not a revival of the old 442KB static cache verbatim) is now the best-evidenced next
  lever on this whole latency line -- warranted by real board data, not just the 13.22x DRAM-traffic
  redundancy figure on its own.**
  **Bonus finding, isolated csynth, 2026-09-02: caching also shortens PW_FLAT's own per-call pipeline
  latency, a benefit separate from and additional to the read-count reduction.** `PW_FLAT`'s achieved
  II is 1 either way (cached or direct-read), but iteration LATENCY differs: cached variants
  (`<true,true>` 13 cycles, `<false,true>` 14 cycles) vs. direct-read variants (`<true,false>`/
  `<false,false>`, both 22 cycles) -- a BRAM read completes faster than an AXI (`gmem_w`) read even
  though both sustain II=1 throughput. Since `PW_FLAT` is invoked once per `(rt,colt)` spatial tile,
  this fill/drain saving (9 cycles) recurs on every tile call, not just once per layer -- small in
  absolute terms (9 cycles x 256 tiles = 2,304 cycles = ~0.023ms for an entry3-scale layer) but real
  and correctly-directional, on top of the much larger DRAM-traffic-reduction benefit.
- **When a real-board measurement comes from a build whose P&R never closed timing, don't just discard
  it OR trust it at face value -- cross-check with a SECOND, physically different implementation of
  the same source and see if the result is bit-identical.** Confirmed useful 2026-09-02 (ZHR-92,
  `PW_FIX_WADDR` weight-residency probe, see the entry above): the route-only build (WNS=-0.944ns) and
  the same checkpoint after `phys_opt_design` (WNS=-0.723ns, still negative but a measurably different
  physical placement/routing) are two different implementations of identical source. Both were board-
  tested 3x each (6 runs total) and came back with the SAME timing (22.63-22.69ms) and the EXACT SAME
  mismatch count bit-for-bit (164,054/196,608, every single run). **The diagnostic logic: a genuine
  timing violation (setup/hold failure, metastability) is a physical phenomenon sensitive to the exact
  delay values on the violating path -- change the placement/routing and a real violation's effect
  (which specific bits glitch, whether it glitches at all) would be expected to shift, at least
  somewhat, not reproduce exactly.** Getting bit-identical wrong output and matching timing across two
  measurably different physical builds is instead the signature of deterministic RTL/logic behavior
  (the circuit is doing exactly what its logic says, just with a deliberately-wrong address in this
  case) that happens not to actually trip the marginal violation on real silicon, even though static
  timing analysis (inherently conservative, worst-case-corner) flags it. This doesn't retroactively
  make the build "timing-closed" -- treat the result with correspondingly lower confidence than a
  clean WNS-positive build, not as fully certain -- but it converts "unusable, discard" into "usable
  evidence, flagged," which is a real difference when the alternative is having no data point at all
  for the question being asked. General applicability: any time a probe/experimental build (not a
  deployment candidate) comes back timing-negative and its own board result looks like real, decisive
  signal, this two-implementation bit-identity check is a cheap (no new synthesis, just re-place/route
  or re-run `phys_opt_design` on the same checkpoint) way to tell a real effect from a timing artifact
  before either trusting or discarding the number.
- Any Vivado run: **background + poll logs, never wait on a full P&R in the foreground.**
  `phys_opt_design` gets silently killed under foreground execution in this environment (see ZHR-17)
  with no crash log — if a run needs `phys_opt_design`, especially post-route, split into two batch
  invocations (route_design in one, then re-open the checkpoint for `phys_opt_design` with
  `set_param general.maxThreads 1` in a second) — this two-phase/single-threaded recipe is proven to
  work on the first try where multithreaded foreground attempts fail silently every time.
- **`export_design` silently reuses cached HDL when called again in the same HLS solution, even if
  `csynth_design` genuinely re-ran on changed source.** Same bug class as ZHR-5's original finding
  ("Vitis HLS 2024.2's `export_design` silently fails to update the top-level RTL" — the fix then was
  "rm -rf the whole project directory before export"). Symptom is the worst kind: two P&R runs on
  *different* source produced bit-identical WNS and identical primitive counts down to the last DSP —
  looked exactly like "the code change had no timing effect," and only grepping the exported HDL for a
  string that should have disappeared (a removed array's name) exposed that P&R had synthesized stale
  RTL both times. One rule, not two: **`export_design`'s target directory must be verifiably clean
  before every export** — either `rm -rf` it first, or export into a brand-new solution name each
  time. Do not reuse an HLS solution across more than one `csynth_design`+`export_design` cycle and
  assume `export_design` picked up the latest source; confirm it did (e.g. grep the exported
  `hdl/verilog` for a signature unique to the current change) before trusting any P&R number that comes
  out of it.
- **`vitis_hls`'s shell exit code does not mean `csynth_design` succeeded.** Confirmed 2026-08-24
  (ZHR-92, `hls::burst_maxi` same-bundle probe): `csynth_design` hit an internal LLVM-IR codegen crash
  ("Call parameter type does not match function signature!" / "Broken module found, compilation
  aborted!") and produced no `mac_array_top_csynth.rpt` at all — but the wrapper script still exited 0,
  because the driving tcl's own trailing `exit` command runs regardless of whether `csynth_design`
  silently aborted internally. Third occurrence of the same failure class in this project ("tool
  reports success, didn't actually do the work") — see `export_design`'s stale-HDL-reuse finding
  directly above, and ZHR-5's original version of it. **Never trust a shell exit code alone as proof
  an HLS/Vivado step completed** — after every `csynth_design`/`export_design`/P&R run, confirm the
  actual output artifact exists and is fresh (the `_csynth.rpt`, the exported `hdl/verilog`, the
  routed `.dcp`) before reading any number out of it or reporting a round's result.
- **A runtime-derived expression used as a loop bound becomes real hardware arithmetic (often a
  multiplier), not just a comparator.** Confirmed by direct measurement, not inference: converting
  four staging loops from runtime bounds (`patch_r`/`patch_c` = `(MAC_PR-1)*S+K`, `K` itself) to
  compile-time constants predicted a DSP drop from csynth (90→63, -27) that real P&R matched exactly
  (63). The loop-bound expressions themselves — not just the write-enable logic those bounds gated —
  were being synthesized into hardware every time they appeared in a loop's exit condition. Any
  derived-bound expression (`(dim-1)*stride+k`-shaped or similar) sitting in a loop bound is a
  candidate for this, independent of whether the loop body writes into a partitioned register array.
- **A runtime value gating entry to a critical hardware region costs real hardware every time,
  regardless of which *form* the gate takes.** This is one principle, confirmed via six independently
  discovered, differently-shaped instances on this codebase — don't search for just "loop bounds";
  search for *any* runtime decision sitting between a descriptor field and a resource-sensitive
  region:
    - round 5: `for (ci < Cin)` as a loop's own exit condition → `PIPELINE` silently dropped entirely
      ("Cannot unroll loop ... variable trip count").
    - round 8: `if (dd >= chunk_sz) continue` guarding a write into an unrolled array → 16,840 LUT of
      Expression/Multiplexer logic (one mux tree per lane).
    - round 12: `dw_patch[rr*dw_S+kh][...]` — a runtime stride folded into an index feeding a 512-wide
      unrolled read → ~3,000+ sparsemux cores just to hold II=1.
    - round 13: `kh = step / MAX_K` — an induction variable *derived* from a flat runtime counter,
      feeding the same kind of unrolled read → the same sparsemux fan-out, different surface syntax.
    - DW fpg fix (2026-08-22): `for (cc < c_sz)` as a loop's own trip count (not just a body guard) →
      not a resource cost this time but a *correctness* one — csim can't see it, only real P&R timing
      exposed the board-measured 46% mismatch (the razor-thin `c_sz` timing path failing to reach a
      2nd real iteration in silicon).
    - WRITEOUT burst-miss (2026-08-23, ZHR-92): `if (rr >= r_sz || cw >= col_sz) continue` wrapping a
      store → HLS's burst inferencer refuses categorically ("Access store is in the conditional
      branch", confirmed via `burst.xml`'s own `AccessInCondBranchMissed` diagnostic, not inferred
      from cycle counts) — independent of how regular the underlying address pattern is.
  Six different syntactic shapes (a loop's own bound, an `if...continue` guard, a dynamic array index,
  a derived induction variable, a loop bound again in a different context, a guard around a store
  instead of a compute), same single mechanism, same single fix every time: hoist the runtime decision
  to a **compile-time-bounded loop with the decision pushed into a data-path `valid`/select**, not left
  in control flow. The two known exceptions to "just zero-fill the invalid case," both from real
  instances above: (1) a *store* can't be zero-filled the way a *read* can — writing unconditionally
  would corrupt memory outside the valid region, not just compute a discarded value, so the fix needs
  an actual fast/slow dual path (full-region fast path unconditional, partial-region slow path keeps
  the guard) rather than a single always-valid rewrite; (2) a loop bound that's also a genuine trip
  count (not just a body-level guard) can produce silently wrong results in real silicon that csim
  never sees, not just a resource-cost regression — treat any "the loop's own iteration count depends
  on a runtime value" case as a correctness risk to verify on real hardware, not just an efficiency one
  to optimize later.
- **Related but distinct mechanism, confirmed 2026-09-01 (ZHR-92, PW tiling proposal Step 2
  writeout-LUT investigation): a runtime-PARAMETERIZED arithmetic operation (not a gate/guard) inside
  an `UNROLL`ed loop gets instantiated once PER LANE, not shared/time-multiplexed the way a sequential
  loop lets HLS bind one physical unit across iterations.** `pw_flat_tile_v2_writeout_group`'s
  `clip_shift(total, shift)` -- `shift` is a genuine runtime value (`d.use_shift_table ? pw_shift_cache
  [oc] : d.out_shift`), and the `acc >> shift` inside it needs a real variable-amount barrel-shifter,
  not a fixed shift -- sits inside a `for (cw < MAC_PC) { #pragma HLS UNROLL ... }` loop (4-wide). csynth
  confirmed 4 separate `ashr_ln79_*`/`shl_ln79_*` instances (~200 LUT each, ~800 LUT just for the
  shift stage, plus matching per-lane `select`/`icmp` clamp logic) -- the SAME shift operation, on the
  same runtime `shift` value, paid 4 times. The deployed `pw_flat_pipeline_impl`'s own writeout calls
  the identical `clip_shift` but with `wr_col` as a SEQUENTIAL loop-carried variable, not unrolled --
  one cell written per cycle, so HLS binds ONE physical shifter and time-multiplexes it across the 4
  columns, instead of building 4. Root cause of a real, measured LUT gap (11,628 combined for 2
  writeout template instances vs the deployed mechanism's own +12.6%-total precedent for the
  *identical* clip_shift/writeout job) -- confirmed via the actual `Expression`-row instance list, not
  inferred; the `Multiplexer` row was small (108 LUT), ruling out an `acc`-indexing mux as the cause
  despite that being the more obvious first suspicion. **Unrolling a loop whose body calls a function
  parameterized by a genuine runtime value is not free just because the function itself is small and
  the loop trip count is small (4) -- check whether the runtime-parameterized operation could instead
  run on a sequential (non-unrolled) version of the same loop, letting HLS share one physical instance,
  before accepting an unroll's parallelism as a strict win.**
  **CORRECTED same day, ATTEMPTED AND REVERTED: simply removing the `#pragma HLS UNROLL` does NOT
  achieve this in practice.** HLS's own loop-flattening/auto-pipelining optimizer re-flattened the
  now-nominally-sequential `cw` loop back into the exact same fully-parallel structure to hit the same
  II=1 target -- achieved II and Iteration Latency were IDENTICAL (1/14) before and after removing the
  pragma, and csynth's own Expression-instance list still showed 4 separate `ashr_ln79_*`/`shl_ln79_*`
  after the change. Total isolated LUT moved only 34,114->33,095 (-3%), not the ~1,600+ a true
  single-shifter fix would give. The deployed `pw_flat_pipeline_impl`'s own sharing isn't just "not
  marked UNROLL" -- its writeout lives in a fundamentally different loop shape (a single flat
  gather/writeout state machine, not a `WRITEOUT_M`/`WRITEOUT_ROW`/`cw` nested-loop structure HLS's own
  flattening pass can re-parallelize), so the same "auto-flatten small bounded loops for II=1" temptation
  never applies there. **For a small (trip count ~4), statically-bounded inner loop feeding an outer
  pipelined region, removing `#pragma HLS UNROLL` alone is not sufficient to stop HLS from re-unrolling
  it anyway -- verify via the actual Expression-instance list (not just the loop table's own II/latency
  columns, which can read identically either way) before trusting that a pragma removal changed
  anything. A real fix likely needs an explicit resource-sharing directive (`#pragma HLS ALLOCATION
  operation instances=... limit=N`) or a loop restructuring that removes the flattening opportunity
  entirely, not a bare pragma removal.**
- **A third confirmed failure shape for an HLS resource-sharing fix, distinct from a silent tool
  bug and from an outright syntax error: a pragma can be SYNTACTICALLY ACCEPTED (no warning, no
  error, "Checking Pragmas" step completes clean) and have ZERO measurable effect on the actual
  binding result.** Confirmed 2026-09-01 (ZHR-92, PW tiling proposal Step 2 writeout-LUT fix
  attempt): `#pragma HLS ALLOCATION operation instances=ashr limit=1` / `instances=shl limit=1`,
  scoped inside `pw_flat_tile_v2_writeout_group` specifically to avoid touching
  `pw_flat_tile_v2_compute_group`'s own multiply/accumulate, produced NO warning and NO error, but
  the csynth Expression-instance list still showed exactly 4 separate `ashr_ln79_*` and 4 separate
  `shl_ln79_*` instances afterward (identical to before), and total isolated LUT was unchanged
  (34,114, bit-for-bit the same as before the pragma). This is a THIRD distinct failure shape on
  this project's own list of "tool doesn't do what it appears to claim" instances: unlike the
  DATAFLOW form-(b) case (which at least emitted `HLS 200-1449`/`200-1450` warnings pointing at the
  actual risk) and unlike the `impl=dsp48` keyword-typo case (which errored outright), this one gave
  **no signal of any kind** that the directive had no effect -- the only way to know was to
  re-inspect the Expression-instance list directly. Root cause not confirmed (candidates: `ashr`/
  `shl` may not be the operation keywords Vitis HLS's allocation-binding pass actually recognizes for
  primitive shift ops, as opposed to core-mapped ops like `mul`/`div`; or the 4 instances, having
  already been produced by `#pragma HLS UNROLL`, may no longer be visible to the allocation pass as
  interchangeable candidates for the same physical resource pool by the time binding runs). **Do not
  treat a clean "Checking Pragmas" pass as evidence that an ALLOCATION (or similarly binding-level)
  pragma actually did anything -- verify against the actual Expression/Instance list every time,
  the same discipline already required for burst-inference and DATAFLOW-process claims.**
  Separately, self-inflicted and worth flagging for future editing of this codebase's own comments:
  writing `ashr_ln79_*` immediately followed by `/shl_ln79_*` inside a `/* ... */` block comment forms
  a literal `*/` at the seam, closing the comment early and turning the rest into malformed code --
  caused two real compile failures this same round before being caught. When a comment needs to name
  two wildcard-style identifiers adjacent to each other with a `*` before a `/`, insert a space or
  reword to break the accidental `*/` sequence.
- **The old, replaced mechanism coexisting with a new one in the same synthesized design is a real,
  precedented risk class on this project (the `pw_pack` export bug traced to a first-time 3-file
  compilation-unit merge) -- but it is a checkable hypothesis, not a foregone conclusion, and this
  round it was checked and RULED OUT.** Confirmed 2026-09-01 (ZHR-92, investigating the 46,259-
  isolated / 92,907-real 2x gap): grepped the actual exported RTL file listing (not just the C++
  source) for the old `pw_flat_pipeline`-family module names -- zero matches; only `pw_flat_tile_v2_
  impl_true`/`_false` modules exist. A real, whole-IP hierarchical utilization report (`report_
  utilization -hierarchical` on the post-synth checkpoint, not the isolated per-function csynth
  report) confirmed the same: `run_layer`'s own 82,797 of the top's 91,522 LUT is accounted for by
  its real children (68,425 combined for the two PW template instances, 11,812 for DW's own
  `run_dw_layer_raster`, the small remainder being ROW_READ/COPY_FROM_ROW/bias-shift-hoist glue) --
  no unexplained hidden hierarchy branch, no orphaned old-mechanism instance. **The real, still-open
  finding from this same hierarchical report: `COMPUTE_M_COMPUTE_N` alone (the weight-tile MAC loop,
  unchanged between the pre- and post-writeout-restructure versions) cost 22,628 + 22,748 = 45,376
  LUT combined for the two template instances in REAL synthesis -- nearly matching this whole
  mechanism's ENTIRE isolated-csynth total for both instances combined (46,259). This means COMPUTE's
  own real-to-isolated inflation is far worse than the design's overall ~2x average, concentrated in
  one specific region** -- and because `compute_group`'s logic is materially the same in the
  restructured (single-instance) version, this is a live, unresolved risk that a real P&R attempt on
  the restructured design could still fail for a genuinely different reason than template
  duplication, even though the template-duplication mechanism itself is now fixed. Not yet resolved;
  flagged as the open question before attempting P&R again.
- **`csynth`'s Performance Estimates only report cycle counts for named `PIPELINE`/`UNROLL` regions
  — a function's own sequential glue code between those regions has no report of its own, and may be
  the dominant real cost.** Confirmed 2026-08-23 (ZHR-92): `run_layer`'s per-tile board time grew
  70.08ms→96.00ms (+37%, +421.9 cycles/tile, uniform across all 6,144 real tiles) after a change that
  touched only address-arithmetic in `DW_PATCH_STAGE`/`DW_WT_STAGE`/`WRITEOUT_DW` — but every one of
  those NAMED regions' own csynth report showed identical or *improved* per-call latency between the
  two versions (`DW_WT_STAGE` even got faster, 20→15 cycles). The +422 cycles/tile is real (board-
  measured) but invisible in every per-region report checked — it has to be in the plain sequential
  code connecting the regions (here: two new outer-scope seed-multiply computations, each a new
  distinct call site for a resource this design already binds to one shared physical multiplier via
  FSM-state muxing — see the DSP-sharing finding below). This is the specific mechanism behind three
  separate failed pre-registrations on the same line (gmem_act widening: predicted PW≈83ms, got
  171.76ms; PW_STAGE elimination: predicted PW≈50ms, got 133.74ms; DW whole-block-burst: predicted
  ~66ms, got a 21% regression) — not three unrelated misses. A csynth region-level prediction's
  *direction* ("which region is relatively more expensive") has held up every time; its *magnitude*
  has not, because it silently omits whatever cost lives in the glue. Don't pre-register a specific
  ms/cycle target from csynth region numbers alone — say the direction, flag the magnitude as
  unverified, and let real P&R + board measurement supply the number.
- **This design binds multiple distinct `index * runtime-stride` address computations across
  `run_layer` (DW_PATCH_STAGE, DW_WT_STAGE, WRITEOUT_DW, WRITEOUT_PW, PW_PATCH_HOIST) to ONE shared
  physical 32-bit multiplier, arbitrated by FSM state** (confirmed via P&R critical path across three
  independent rounds: `ap_CS_fsm_reg[...] → mul_32s_32s_32_2_1`'s cascade register, logic levels
  climbing 4→4→5 as more call sites were added, `mul_32s_32s_32_2_1`'s instance count in `run_layer`
  staying at exactly 1 even after an accumulator rewrite reduced how many times each site's multiply
  gets *evaluated*). Reducing a call site's own evaluation count (loop-invariant hoisting) does not
  by itself reduce how many *distinct* call sites are competing for the shared multiplier — it can
  even add new ones (an outer-scope seed multiply is a new site). Each additional site is suspected
  (not yet confirmed via RTL cosim — unused anywhere in this project so far) to cost real FSM
  arbitration overhead per invocation, independent of the multiply's own 3-4 cycle latency.
  **CONFIRMED, not just suspected, 2026-08-29 (ZHR-92, descriptor-hoist round):** removing 2 redundant
  per-tile `gmem_meta` reads from `pw_flat_pipeline_impl` (pure subtraction, no new call sites added by
  hand) real-P&R'd into a WORSE WNS (+0.021ns → -0.305ns), and the critical path *moved* — for the
  first time across an entire exploration line (baseline, 111MHz, 125MHz, and an X0-108 pblock attempt
  had all shown the identical `gmem_meta` AXI-FIFO path, ~10ns/10 logic levels/80-83% route, remarkably
  stable) — to `pw_flat_pipeline_impl`'s own FSM feeding `grp_fu_873`, the SAME shared-multiplier
  functional-unit index this project has already traced to the scalar-op family (`run_gelu`/
  `run_sigmoid`/`run_relu`/`run_add`) in an unrelated earlier round. HLS's own global binding decision
  added a new competitor to that shared resource as a *side effect* of an unrelated local code change
  that only removed reads — the mechanism this bullet already predicted, now caught in the act via a
  real, unplanned P&R result, not induced by design.
- All results — including negative ones — get written back to the relevant Linear issue as a
  comment, not just left in chat or local memory. Real numbers over assumptions: this project has
  been burned before by static-report/simulation readings that turned out wrong (ZHR-5's "140x
  mystery" — 6 of 8 leads were misled by static analysis, only real bitstream sweeps found the true
  cause). When a question can be answered by running real hardware instead of reasoning about code,
  run the hardware.
- Board safety: the currently-deployed bitstream is the golden rollback image — never overwrite it
  without being told to. Any new binary/bitstream gets a small isolated test before a full-network
  run (ZHR-10: a change that was "HLS/Vivado all-green" hung the real board).
- **Vivado's own `write_bitstream -bin_file` output is NOT the byte-swapped format the Zynq-7000
  devcfg FPGA manager driver requires.** Confirmed 2026-08-24 (ZHR-92): loading it directly failed
  with "Invalid bitstream, could not find a sync word. Bitstream must be a byte swapped .bin file"
  (from `dmesg`, not guessed). The actual required conversion is `fpga_overlay.py`'s own `bit_to_bin()`
  function (word-swaps the `.bit` file's payload) — every previously-archived bitstream's
  `_swapped.bin` file already went through this; Vivado's `-bin_file` flag output was never the
  deployable artifact on its own. Always convert via `bit_to_bin()` (or pull the swapped `.bin` back
  off the board after a successful load, as this project's archived bitstreams already do) before
  copying anything into `/lib/firmware` — never assume Vivado's raw `-bin_file` output is load-ready.
- **Sharing an m_axi `bundle=` between a plain pointer and an `hls::burst_maxi`-typed parameter does
  NOT mean they share a control register.** Confirmed 2026-08-24 (ZHR-92): `out_burst` and `out_base`
  both bind to `bundle=gmem_act` (same physical AXI master, confirmed via csynth — no 5th master), but
  HLS still allocates `out_burst` its own separate AXI-Lite base-address register (offset `0x6c`/`0x70`
  in this design, read from the solution's own generated `xmac_array_top_hw.h`, never guessed). If the
  ARM host only programs the plain-pointer parameter's register and leaves the `burst_maxi` parameter's
  register unprogrammed, every write through the burst path lands at whatever that register defaults
  to — wrong output, not a crash, and **csim cannot catch this at all** (csim has no register-address
  concept; it just calls the C++ function directly with real pointers). Any new `hls::burst_maxi`
  parameter needs its own register write added to the ARM driver, verified by finding its real offset
  in the generated hardware header — never assumed from "it shares a bundle with X, so it must share
  X's register."
- **A PL hang can poison the PS side's own reads of `/lib/firmware`: file size and mtime stay
  unchanged, but the content that reads back is wrong.** Confirmed 2026-08-24 (ZHR-92): after a
  full-network run hung (PID alive, zero CPU time, no progress) and was killed, `md5sum` on a `.bin`
  file already verified correct twice — including immediately before the hang — started returning a
  THIRD, different checksum, reproducibly, with unchanged size/mtime. A full power cycle (not a soft
  `reboot` — a hard power pull) fixed it: the file read back correct again with no re-copy needed,
  confirming this was a stale/volatile PS-side read, not real on-disk corruption. **Any MD5 check
  performed after a hang or a failed FPGA reconfiguration attempt is not trustworthy** — power-cycle
  the board first, then re-verify, before trusting any checksum read from it. Fourth confirmed instance
  of "tool/system reports something that isn't true" in this project (after `export_design`'s
  stale-HDL cache reuse, `vitis_hls`'s misleading exit code, and ZHR-5's original finding) — the common
  thread: never trust a report of state without independently confirming the underlying artifact, and
  add "was there a hang or failed reconfiguration since the last known-good state" to the list of
  reasons a report might lie. **Caveat added 2026-08-25**: don't assume the causal direction is
  settled either. The hang and the MD5 anomaly were read as hang→corruption (in that order), but 11/11
  later reproduction attempts (10 repeated full-network runs plus 1 replaying the exact original
  pre-hang command sequence) failed to reproduce the hang at all — which means the original hang and
  the MD5 anomaly may both have been downstream symptoms of one earlier, still-unidentified cause,
  not a simple chain where the hang caused the bad read. Don't assume the first plausible causal story
  is the right one just because the timing lines up.
- **Board recovery checklist after a hang or failed FPGA reconfiguration — follow in order, one step
  at a time, don't skip ahead even if a step "obviously" will pass.** Established 2026-08-24/25
  (ZHR-92) after the first occurrence was worked out live under pressure; codified here so the next
  one doesn't require re-deriving it:
    1. **Full physical power cycle** — not a soft `reboot`. A software reboot does not reliably reset
       the FPGA manager / PL fabric / DDR controller state the way a hard power pull does. This
       project's agent has no remote power control — ask the user to do it, then verify via `uptime`
       (should read minutes, not hours — confirmed useful 2026-08-25: an assumed-fresh board turned
       out to have 15h35m uptime, i.e. no power cycle had actually happened yet, caught only by
       checking this number instead of trusting the claim).
    2. **Load the golden rollback image first**, verify its MD5 and `fpga_manager` state — this is the
       cheapest possible proof the PL fabric itself can still be configured at all, before spending any
       effort on the bitstream actually under test.
    3. **Re-verify the target bitstream's MD5** — per the bullet above, do not trust any MD5 read
       before this point (i.e. before both the power cycle and the golden-image proof-of-life).
    4. **Re-deploy from the local archive** (`vivado_impl/bitstream_archive/<name>/`), not from
       whatever copy is already sitting in the board's `/lib/firmware` — even if its MD5 now reads
       correct, treat "was this file touched by anything since the hang" as unverifiable and replace it
       anyway.
    5. **Single-op verification** (a known-good isolated test, e.g. one already-passing entry) before
       trusting the board for anything larger — confirms the specific re-deployed bitstream actually
       works, not just that the FPGA manager accepted it.
- When the code itself contains an admitted placeholder/TODO (a hardcoded stand-in value, a comment
  saying "not yet calibrated"/"not yet implemented", etc.) and the observed symptom is consistent with
  that placeholder being the cause, verify the placeholder first — before chasing a more interesting
  or more specific-sounding hypothesis. `calibrate_activations.py`'s `default_act_scale=1/127` was
  flagged as Phase 0.7's own step 3 at kickoff ("replace the placeholder with real calibration") and
  then deferred through 9+ debugging rounds while more specific theories (SE `out_shift`, a missing
  final GELU, LayerScale) got chased instead — it turned out to be the dominant root cause, off by
  ~37x, confirmed only in Phase 0.8 step 5 by finally checking it directly.
- **A golden/reference implementation written in a different language from the thing it checks can
  silently diverge on integer-division truncation direction, not just on obvious algorithm
  differences.** Confirmed 2026-08-26 (ZHR-63 quantization-fix round, building
  `diagnose_full_network_sim.py`'s bit-faithful Python re-implementation of `mac_array.cpp`): C++'s
  `sum/HW` on a signed integer truncates toward zero; Python/NumPy's `//` floors toward −∞. These
  give different results whenever the dividend is negative — exactly the case here, since `run_gap`
  sums signed int8 activations that can net negative. Undetected, this would have made the simulator
  systematically diverge from real hardware immediately after every GAP op (the SE block, entries
  75+) — and because the divergence looks like ordinary quantization error (small, plausible integer
  drift), it would have been mistaken for a real accuracy contributor and chased as one, not caught
  as a tooling bug. Caught only because the simulator's output was checked against real hardware
  dumps entry-by-entry before being trusted (same discipline as `diagnose_entry_by_entry.py`'s
  original bit-exact validation), not because the bug was anticipated. **Any time a golden/reference
  model is written in a different language than the implementation it validates, integer division,
  modulo, and shift semantics need an explicit sign-behavior check — "translated the algorithm"
  is not the same claim as "translated the arithmetic," and the two can differ exactly on the inputs
  most likely to matter (real signed data, not synthetic all-positive test vectors).**

- **A plan recorded in Linear ("do X next") is not evidence that X was executed — check the actual
  code/artifact before citing a past round's conclusion as fact.** Confirmed 2026-08-26 (ZHR-63
  quantization-fix round): Phase 0.7's kickoff plan (2026-08-15) said "run activation calibration on
  real images." Every later round — including repeated verbal instructions across this whole
  project's history — referred to this as something already done. It never was: Phase 0.8's actual
  calibration fix (`calibrate_and_requantize_256.py`) explicitly documents, in its own docstring,
  that it uses the same synthetic generator as `calibrate_activations.py` ("this repo has no
  photos"). The "real image range [-1.434, 1.748], 14.1% saturation" figure cited in multiple later
  rounds as if it came from a real photograph was *also* measured on that same synthetic generator's
  own output. Nobody had checked the actual data source in over a dozen rounds; the plan's own
  wording was being read as a completed-work log. **Before citing a prior round's finding, especially
  one restated secondhand across several later comments, check what the code that produced it
  actually consumed — a plan entry, a comment saying "next we should," or a docstring's stated intent
  is not the same claim as "this ran."**
- **A file's own header comment claiming a test result ("csim-verified N/N") is a claim, not proof —
  it can go unverified for a long time before anyone actually re-runs it.** Confirmed 2026-08-30
  (ZHR-92, gmem_meta round): `dw_raster_layer_tb.cpp`'s own header comment claimed "csim-verified 5/5
  against real layer data" since 2026-08-28, cited secondhand in `mac_array_raster_integrated.cpp`'s
  own header comment ("run_dw_layer_raster()... csim-verified 5/5 against real layer data") without
  anyone actually running it again in between. When the gmem_meta interface change broke this file's
  own `mac_array_top()` call site (old signature), fixing it and actually running it for the first
  time in this investigation reproduced 5/5 PASS — the claim held up, but it had gone unverified for
  two days across multiple intervening rounds that cited it. Same class as the plan-vs-execution gap
  above and the `mac_array_tb.cpp`-vs-raster-file pairing confusion earlier in this same round (where
  a DIFFERENT long-cited number, "17/17", turned out to belong to a different source file entirely) —
  three instances now of a stated test-count claim in this project needing an actual re-run before
  being trusted, not just re-cited. **A "verified N/N" claim in a comment or commit message is only as
  fresh as the last time someone actually executed it — treat any such claim as due for re-verification
  before relying on it in a new decision, especially after an interface change that could plausibly
  affect the file being claimed about, even if that file wasn't the direct target of the change.**
  **Highest-cost instance yet, confirmed 2026-09-01 (ZHR-92/ZHR-63, PW tiling "make room" round,
  DW FPG-specialization attempt): `dw_raster_layer.cpp`'s own `DWR_ENABLE_FPG_SPECIALIZATION`
  ("Option A") comment claimed, since it was written, "true II=1 for fpg=1, but real measured cost
  is 93.6% DSP" -- rejected for DSP budget, not for II. Nobody re-ran this flag across several
  intervening rounds that materially touched this same file (`DWR_HOIST_BASE_ADDR`, `LB_FORCE_DSP`,
  the writeout `template<bool>` split) before a whole new round was DESIGNED around this stale
  number (the "reduce DW parallelism instead of migrating it to the ARM" line, chosen specifically
  because it looked cheap and fast relative to the other two closed options). Fresh re-measurement
  reproduced neither claim: LUT went 11,543->28,017 (+143%, the opposite of the intended saving) and
  achieved II regressed to 4 (FPG_MODE=1) and 8 (FPG_MODE=2), both worse than the deployed default's
  own II=1 -- not "still 93.6% DSP but otherwise as claimed," a different and worse failure entirely.
  **Operational rule going forward: any mechanism gated behind an `#ifdef` (a variant not on the
  default/deployed build path) carries a historical performance number that must be RE-MEASURED
  before being cited or relied on for a new decision -- never just re-quoted from its own comment.**
  The reason this class of staleness is invisible by default: code behind an inactive `#ifdef`
  doesn't participate in the project's own everyday csim/csynth/P&R runs (which all build the
  default path), so nothing else in the normal workflow would ever re-exercise it or surface a
  drift -- unlike a `csim-verified N/N` claim on ACTIVE code (which at least risks being silently
  invalidated by a compile error the next time someone touches that file), a disabled `#ifdef`
  branch can go arbitrarily long without ANY signal that its own numbers stopped being true.

- **A number a reviewer/user states as a result is not evidence unless it was actually produced by a
  tool run you can point to — do not write it into a permanent record (Linear, CLAUDE.md) until you've
  reproduced it yourself, and say so explicitly if you can't.** Confirmed 2026-08-26 (ZHR-92,
  per-channel-vs-pooled-calibration round): the user stated specific, precise-looking numbers
  ("saturation 23.47%→0.19%, 48/48 layers' best_delta→0, stage1 cosine 0.6313→0.6482") plus a named
  bug mechanism ("sum(...)/n averaging by total element count instead of per-channel count"), framed
  as an already-confirmed root cause. The actual tool run produced different numbers in the OPPOSITE
  direction (saturation got worse, 23.47%→25.90%; stage1 cosine got worse, 0.6313→0.5161). Refusing to
  write the stated numbers into ZHR-92 and asking where they came from — instead of accepting them and
  building the next round on top of them — surfaced that the user had fabricated the numbers by
  inference from conversational context ("this is the result that should happen"), not from having
  actually run anything; they confirmed this directly the next turn. Had the numbers been written down
  as fact, every subsequent round would have reasoned from fictional data. **Any claimed result —
  from a user, a teammate, a review comment, or your own memory of an earlier round — needs an
  actual artifact (a log, a report file, a rerunnable command) before it goes into a permanent record.
  "I should independently reproduce this, not just transcribe it" applies regardless of how confident
  or specific the source sounds.**

- **A persisted intermediate artifact can look valid (right file, right size, right dtype) while being
  generated under a configuration the current code no longer uses — and nothing checks.** Confirmed
  2026-08-26 (ZHR-92, quantization-fix round): `accuracy_test_imgs_256/stem_output_0000.bin` — the
  Stem output every simulator script AND the real board's `mac_array_full_network_test.c` (line 154,
  `load_file(stem_path, arena_v, stem_size)` loads it straight into the DMA arena dispatched to the
  FPGA) used as the network's starting point — was quantized with `compute_stem_arm.py`'s **default**
  `--output-scale` (`None` → falls back to `PLACEHOLDER_SCALE = 1/127`), not the real calibrated
  `stem_output_scale = 0.132136` every downstream shift/scale in `shift_table_meta.json` assumes. This
  made the file **71.6% saturated** at ±127/±128 — confirmed empirically (69% exact match quantizing
  the true ONNX Stem value at 1/127, 0% match at the real 0.132136), not inferred. Fourth confirmed
  instance of this project's "tool/artifact reports something that isn't true" failure class, alongside
  `export_design`'s stale-HDL-cache reuse, `vitis_hls`'s misleading exit code, and the PL-hang MD5
  anomaly — the common thread across all four: **a persisted artifact that looks legitimate (right
  shape, plausible values, no error) was generated under a since-superseded configuration, and the code
  consuming it has no way to know.** Regenerating it correctly (matching `shift_table_meta.json`'s
  real `stem_input_scale`/`stem_output_scale`, plus a *second*, independent stale artifact discovered
  along the way — `compute_stem_arm.py`'s own default `--image img_0000.bin` is a wrong-resolution
  leftover, 128×128 vs this pipeline's 256×256, which would have crashed the script's reshape had it
  actually been used) fixed the saturation (71.6%→0%) but — checked directly, not assumed — did **not**
  meaningfully change the six-checkpoint cosine curve (stage1 0.6313→0.6325, cliff shape unchanged) or
  explain the accuracy line's actual open questions; this project's independently-confirmed network-wide
  per-layer saturation (48/48 layers, 23-37%, unrelated to Stem) dominates enough that a clean Stem
  start doesn't move the needle. Both facts matter and don't cancel each other: the artifact bug was
  real and board-relevant (not simulator-only), *and* it wasn't the explanation being sought. **General
  guard, not yet applied everywhere it should be**: any persisted intermediate artifact (a calibration
  output, a golden reference, a precomputed table) that another script consumes without regenerating
  needs its generating parameters recorded alongside it (a sibling `.meta.json` — added this round for
  `stem_output_0000.bin`, listing `input_scale`/`output_scale`/generator/commit/image-source) and
  ideally checked at load time, not trusted because the file exists and parses. This is expensive to
  retrofit everywhere at once; treat it as the default going forward for any NEW persisted artifact,
  and retrofit existing ones opportunistically when touched, not as a dedicated sweep.
- **A standalone HLS probe's own unpartitioned arrays can silently masquerade as a limit of the
  mechanism under test, not a property of the probe.** Confirmed twice in immediate succession,
  2026-08-27 (ZHR-92, line-buffer and DSP-pack-array rounds): the line-buffer probe's synthetic input
  array (`pixels[MAC_PD][N_PIXELS]`) and, one round later, the DSP-pack-array probe's accumulator
  (`acc[N_GROUPS][2][2]`) were both left unpartitioned, and both times HLS's own diagnostic
  (`HLS 200-885`, quoted directly both times, not inferred) named the exact array and the exact reason
  ("limited memory ports") when multiple unrolled/parallel accesses per cycle contended for a
  default-inferred 1-2-port memory. Both times this looked exactly like a real architectural
  bottleneck (achieved II degrading 1→2→4 as parallelism P grew; a DSP-packed reduction loop
  measuring only ~2x speedup instead of the theoretical 4x) until the diagnostic was read carefully
  and the one-line fix (`#pragma HLS ARRAY_PARTITION variable=<array> complete dim=<N>`) resolved it
  at zero or near-zero resource cost, fully recovering the theoretical result both times. Neither case
  was actually testing the row-buffer/BRAM mechanism or the packed-multiply mechanism's own limits —
  both were testing the probe's own data-feeding/accumulation scaffolding. **Any array touched by
  multiple simultaneous (unrolled/parallel) accesses per cycle — inputs and accumulators alike, not
  just the structure being deliberately tested — needs to be checked for partitioning before
  concluding a measured II/latency bottleneck reflects the architecture under test rather than the
  probe's own construction.** Read the HLS log's own II-violation diagnostic first; it names the
  array directly and makes this a one-line fix, not a redesign.
- **A net resource-increment estimate built from two isolated csynth runs (new mechanism's own
  csynth, minus the old mechanism's named-region csynth numbers, added onto a real P&R baseline) is
  not a substitute for real P&R — confirmed wrong in both magnitude and direction, not just
  magnitude.** Confirmed 2026-08-28 (ZHR-92, DW raster integration Step 2): the forced-DSP isolated
  estimate for the new mechanism (10,825 LUT / 110 DSP; net vs. the replaced DW regions: -2,357 LUT /
  +94 DSP; predicted whole-IP total 26,914 LUT (50.59%) / 149 DSP (67.7%)) was checked against real
  P&R on the actual integrated design and came back **41,222 LUT (77.48%) / 92 DSP (41.82%)** — LUT
  underestimated by 53%, DSP overestimated by 38%, **in opposite directions**, not the uniform
  "csynth runs high, real P&R comes in lower" pattern this project had previously assumed (see the
  0.632x csynth→P&R scaling factor noted elsewhere, itself flagged as "self-assumed... unverified
  per-region" — this result shows why that flag was warranted). Root cause: the estimate's own
  arithmetic crosses two different tools' resource-allocation decisions as if they were one number —
  HLS's csynth reports what *that function's own synthesis* bound to LUT vs. DSP in isolation, but
  real Vivado synthesis (the `launch_runs synth_1` step on the *whole* exported IP) makes its own,
  independent LUT-vs-DSP binding/packing decisions across the *entire* design, which an
  isolated-function csynth run has no way to see or predict. **A net-increment estimate assembled
  this way (isolated-csynth-of-new minus isolated-csynth-of-old, layered onto a real P&R baseline) is
  a *sizing sanity check* at best — useful for deciding whether a design is obviously infeasible, not
  a number to plan a timing/resource budget around. Before committing to a resource-sensitive decision
  (pblock sizing, DSP-vs-LUT binding choice, accept/reject a design), run the real, whole-IP P&R —
  don't extrapolate from isolated-mechanism csynth deltas, even when both the "new" and "old" halves
  were independently csynth-measured with real care.**
  **Third confirmed instance, 2026-08-29 (ZHR-92, DSP-packing Step 2+3→whole-IP check):** this time
  the metric that diverged was II, not resources. Step 3's isolated csynth of
  `pw_flat_pipeline_packed` (compiled alone, one call site) achieved II=1 exactly, matching the
  theoretical trip count. Compiling the SAME unmodified function body inside the full `mac_array_top`
  call graph (csynth only, not yet P&R) made its own `PW_FLAT_PACKED` loop regress to **II=2** — the
  loop's `dsp_pack_mul_signed` calls, isolated to one call site in Step 3, now compete with the rest
  of the design's resource/scheduling context. Net effect: DSP packing's already-modest 2x theoretical
  benefit (only `n_ot` halving; the 16 spatial lanes were already UNROLL-parallel, see the plan's own
  correction on this) collapses to roughly 1x once the II=2 tax is applied, before P&R was even
  attempted — this alone was judged sufficient to abandon the integration without spending an
  export/P&R cycle to confirm it further (see the DSP-packing line's own close-out below). **Three
  distinct metrics now (LUT/DSP split, WNS sign, achieved II) have each independently violated
  "isolated csynth predicts whole-IP behavior" in this project — treat this as the default
  expectation for ANY isolated-csynth number (resource, timing, OR II), not just resources.**
  **Fourth confirmed instance, 2026-08-29 (ZHR-92, ROW_READ burst-batching line-C, Step 4 real P&R):**
  this time the direction of the error itself flipped, not just the magnitude. Isolated csynth
  (Step 3, `row_read_produce`/`row_read_consume` extracted into their own DATAFLOW processes) predicted
  +1,244 LUT / +3 DSP over the deployed baseline. Real whole-IP P&R came back **+172 LUT / +3 DSP** —
  DSP matched exactly, but LUT was overestimated by **7.2x** (1,244 predicted vs. 172 real). The
  DW-raster round's own instance of this same lesson (net-increment estimate) *underestimated* LUT by
  53%; this one *overestimated* LUT by 7.2x — opposite directions, same class of number, same project,
  ~1 day apart. **Four distinct metrics now (LUT/DSP split — twice, in opposite directions — WNS sign,
  achieved II) have each independently violated "isolated csynth predicts whole-IP behavior." The
  direction of the error is not even consistent across instances — don't assume a "conservative"
  isolated-csynth estimate is safe just because a prior instance happened to underestimate; the next
  one may just as easily overestimate by a large factor.**
  **Fifth confirmed instance, 2026-08-29 (ZHR-92, descriptor-hoist round):** a gentler case, but the
  same lesson — isolated csynth predicted -2.7%/-3.7% (LUT/FF) for removing two redundant per-tile
  `gmem_meta` reads; real whole-IP P&R came back -1.26% LUT (DSP/BRAM both correctly predicted at zero
  change). Same *direction* this time, but real magnitude was roughly half the isolated estimate — a
  reminder that even a same-sign isolated-csynth number isn't a magnitude to plan around, not just an
  opposite-sign one.
  **Sixth confirmed instance, 2026-08-29 (ZHR-92/ZHR-8, HLS-reschedule-to-8ns round) — more extreme
  than the first five: this time the isolated-csynth signal was ZERO, not just wrong in magnitude or
  direction.** Re-synthesizing the exact same source (`mac_array_raster_integrated.cpp` +
  `dw_raster_layer.cpp`) at `create_clock -period 8` vs. `-period 10` produced a whole-IP Estimated
  critical-path delay that was **bit-identical to three decimal places (9.676ns) in both builds** —
  csynth reported no rescheduling occurred at all, and the named `PW_FLAT` region's own latency/II
  numbers were flat too (21 vs 22 cycles, noise). Read at face value, this said "HLS rescheduling is a
  dead lever, don't bother with P&R." Real P&R at a matching 125MHz PS7 clock said the opposite: WNS
  improved 43% (-2.129ns → -1.204ns), the critical path's own data-path delay dropped 9.987ns→8.923ns
  (-10.7%), logic levels dropped 10→7, and — for the first time in this entire 150MHz investigation
  line — the critical path moved off `gmem_meta`/the shared multiplier entirely, to a new site
  (`dwr_consume6`'s `CROW_CCOL` comparator feeding `gmem_act`'s store FIFO). The isolated-csynth
  Estimated-timing number is apparently a coarse per-function static estimate that doesn't reflect
  where Vivado's actual synthesis/placement ends up finding slack across the whole design — it is not
  just unreliable in magnitude/direction (instances 1-5), it can show **exactly zero signal** for a
  change real P&R shows is substantial. The only reason this didn't get misreported as "lever
  falsified" is that this project's own prior-instance discipline said to run real P&R regardless of
  what csynth showed — do not let a flat/unchanged isolated-csynth Estimated-timing number alone
  justify skipping the real P&R check, even when (especially when) it looks like clean, decisive
  evidence of no effect.
  **SHARPENED, 2026-09-02 (ZHR-92, PW weight-residency round, BRAM): "isolated csynth is unreliable"
  is an imprecise summary of the pattern above — the precise version is "isolated csynth is
  unreliable AND its error has no consistent sign, so a specific isolated number cannot be assumed
  optimistic or pessimistic just because prior instances leaned one way."** Every instance on this
  list before this one happened to have isolated csynth UNDERSTATE the real cost (DW-raster's LUT,
  ROW_READ's 7.2x LUT overshoot, the DSP-packing II regression) or occasionally overstate it
  (descriptor-hoist's -1.26% vs -2.7% predicted) — but this round's BRAM number is the first case
  where isolated csynth was actually MORE PESSIMISTIC than reality (isolated extrapolation: 98/140
  tiles, 70%; real P&R: 74/140, 52.86%) on a line that had, until this point, only ever been burned
  in the "isolated undersold the real cost" direction. Do not let a run of same-direction misses build
  an implicit expectation for which way the NEXT miss will go — treat every isolated number as
  direction-agnostic-unreliable, not "probably optimistic" or "probably pessimistic" going in.
  **Also new this round: within the SAME build, different resource categories can diverge differently
  from their isolated predictions** — this round's LUT (isolated +725/+1.5%, real +433/+1.37%) and DSP
  numbers tracked their isolated predictions closely, while BRAM (isolated implied +128 units/+64
  tiles, real +40 tiles) did not. A close isolated-to-real match on one resource category is not
  evidence the others will match too; each of LUT/DSP/BRAM/timing needs its own real-P&R confirmation,
  independent of how well the others predicted.
- **An HLS-level resource-binding choice (`#pragma HLS BIND_OP ... impl=DSP` vs. leaving it default)
  does not reliably determine the real, whole-IP P&R resource distribution — but it can still change
  real placement, and therefore real timing, even when it doesn't.** Confirmed 2026-08-28 (ZHR-92, DW
  raster integration Step 2): the same integrated design, re-exported and re-P&R'd with the DW
  reduction's multiply forced to DSP (`LB_FORCE_DSP`) vs. left at HLS's default LUT-preferring
  binding, came back with **essentially identical real resource counts** (LUT 41,222 vs. 41,243, DSP
  92 vs. 92, BRAM 35 vs. 35 tiles — Vivado's own `synth_design` step apparently makes its own DSP-
  inference decision on 8-bit multiplies largely independent of the HLS-level hint). Yet the two runs'
  real timing outcomes were opposite: forced-DSP failed timing (WNS=-0.108ns), the default/LUT-mode
  build met timing (WNS=+0.021ns), on **matching resource totals**. The two builds' differing RTL
  structure (even while converging to similar final resource counts) evidently steered placement
  differently enough to move an already-razor-thin margin across zero. **Treat an HLS-level binding
  pragma as a lever on RTL structure and therefore placement/timing, not as a reliable lever on final
  resource counts — the two effects are separate and can point in different directions; don't assume
  a binding choice "didn't matter" just because a follow-up real-resource check comes back unchanged.**
- **Two independent implementations of the same wait-for-ap_done mechanism can coexist in this
  codebase with only one of them actually on the path that produces this project's cited numbers —
  a third instance of the "two paths do the same thing, only one is real" class (after the
  `use_wide_path` dead field and `mac_run_layers`'s unbounded-wait precedent).** Confirmed 2026-08-28
  (ZHR-92, fixed-dispatch-overhead quantification round): `mac_array_driver.c`'s
  `mac_wait_done_timeout()` polls at `usleep(1000)` (1ms granularity) and is what
  `mac_array_single_op_test.c` calls — but `mac_array_full_network_test.c` (the harness that produced
  every full-network ms figure this project has cited, including the 3,608.76ms/4,288.60ms numbers
  in the DW-raster round) never calls it at all; it has its own separately-written inline poll loop at
  `usleep(500)` (0.5ms). Found while measuring the real per-entry dispatch floor (0.580ms, confirmed
  to ~zero variance over 20 repeated full-network runs) — the floor itself turned out small (≈65ms /
  1.79% of a full run, below the round's own close-out threshold), but the duplicate-implementation
  finding is the more durable lesson: **when asked "what polling granularity does this project use,"
  the honest answer requires checking which specific harness produced the number in question, not
  assuming one canonical driver function is universally on the path** — this codebase has at least two
  real, working, differently-tuned implementations of the same wait loop, and only source-reading
  (not the function's own name or the driver header's documentation) tells you which one a given
  measurement actually went through.
- **A 7th confirmed instance of this project's "tool reports success, didn't do the work" class — but
  a genuinely different failure shape from the first 6.** Confirmed 2026-08-29 (ZHR-92, DSP-packing
  Step 4 export): `export_design` on a design combining three separately-compiled source files for the
  first time (`mac_array_raster_pwpack_integrated.cpp` + `dw_raster_layer.cpp` +
  `pw_pack_pipeline.cpp`) failed downstream Vivado BD synthesis with `module
  'mac_array_top_mul_32s_31s_32_2_1' not found`. Confirmed via direct inspection, not inferred: HLS's
  own csynth log claims "Generating core module 'mul_32s_31s_32_2_1': 2 instance(s)"; grepping the
  entire exported project tree for that filename finds zero matches — it was never written to disk at
  all. What WAS written: `mul_32s_31ns_63_2_1.v` and `mul_32s_32s_32_2_1.v`, neither name matching what
  the failing call site references. Reproduced twice, fresh `rm -rf` + fresh project name both times,
  same result (different `ipshared` cache hash, identical error) — deterministic, not a stale-cache or
  path issue. **This differs from the first 6 instances of this failure class (export_design reusing
  cached HDL, `vitis_hls`'s exit code lying, stale calibration/Stem artifacts, etc.) in kind, not just
  number: those were all "the tool skipped/reused old work instead of doing new work." This one is "the
  tool DID generate new work, but the generated call site's embedded module name doesn't match what
  RTGEN actually named the file it wrote" — a real RTL-codegen naming inconsistency inside a single
  HLS run, not a caching/staleness problem. Don't reach for the stale-cache playbook (rm -rf, fresh
  solution name) for this shape of failure — already tried, confirmed not the cause.**
- **A comment asserting an invariant ("must stay X", "always Y") is a claim someone believed true at
  the time they wrote it, not a currently-enforced guarantee — this codebase has hit the same shape of
  failure enough times that any such comment needs independent verification, not trust, before being
  relied on.** Confirmed again 2026-08-29 (ZHR-92, gmem_meta-to-s_axilite recon): `mac_array_driver.h`'s
  `MacLayerDesc` struct carries the comment "Must stay byte-layout-identical to... LayerDescV2 (int
  fields, same order)" directly above its definition — but it has only 27 `int32_t` fields, one short
  of `LayerDescV2`'s real 28 (`use_wide_path` was appended 2026-08-23 and this driver-side mirror was
  never updated to match). Found by chance while tracing an unrelated question (driver blast-radius for
  an AXI-Lite conversion), not by auditing comments for staleness. Currently harmless only because
  `use_wide_path` happens to be dead code in the active dispatch path — a coincidence, not something
  the comment's claim protected against. This is the same failure shape as the plan-vs-execution gap
  (2026-08-26, "a plan recorded... is not evidence X was executed") and the persisted-artifact-drift
  class (stale Stem calibration, stale image resolution default) already in this file: a stated
  invariant, once true, silently stopped being true when one side of a two-sided contract changed and
  nothing re-checked the other side. **Any comment phrased as an invariant across two independently
  editable locations (two structs that must match, two files that must agree, a cached artifact that
  must reflect current config) is a standing TODO to verify, not a fact — especially before building
  new code (like a register-write driver) that will silently inherit whichever version is wrong.**

## Current deployed baseline (updated 2026-08-31 -- supersedes every earlier baseline reference below)

**`mac_array_a3_gmemmeta_elim1` is now the deployed baseline**, replacing `mac_array_a3_dwraster_step2`
(3,608.76ms/77.52% LUT/WNS+0.021ns, deployed 2026-08-28). Every mention of "3,608.76ms" / "77.52% LUT"
elsewhere in this file below is a **historical value, accurate for the round that measured it** — not
rewritten, since those entries correctly record what was true and known at the time. This section is
the pointer to what's current.

Board-verified 2026-08-31 (ZHR-92, gmem_meta elimination -- see
`vivado_impl/bitstream_archive/mac_array_a3_gmemmeta_elim1_2026-08-31/README.txt` for the full
writeup): gmem_meta master (desc/out_written DMA path) eliminated entirely, moved to `s_axilite`.
Real P&R: **WNS +0.272136ns** (best margin on this whole line), **LUT 31,520/53,200 (59.25%)**, down
from 77.52%, **DSP 48/220 (21.82%)**, down from 41.82%. Board: single-op PW+DW byte-exact, full
network 82/82 written, 6/6 checkpoints byte-exact vs csim (cosine=1.000000), PL total 3,630.74ms
(+0.61% vs the old baseline, essentially flat as predicted -- the eliminated path was config reads,
not the data path).

Practical consequence: the new build has ~18 percentage points more LUT headroom than any prior
config on this line. Lines previously closed out for lack of resource/timing margin (DSP packing,
MAC_PD expansion) may be worth revisiting against this new headroom -- flagged, not re-opened as of
this entry. Register map changed completely (every `s_axi_control` offset shifted -- see the archive
README's table); any ARM-side binary built before 2026-08-31 will silently write to wrong addresses
against this bitstream. Always rebuild the driver/test harnesses from current source before using
them against this build.

## Known open issues as of 2026-08-15

- **OPEN, 2026-09-02: `accuracy_test_imgs_256/ckpt_hw_*_0000.bin` (the full-network checkpoint
  correctness reference `tools/compare_board_full_network_ckpts.py` compares real board dumps
  against) is currently WRONG/stale, and the last time it was known-good is uncertain.** Found while
  board-testing the PW weight-residency mechanism (ZHR-92): a real full-network run of the
  weight-hoist build showed all 6 checkpoints mismatching this reference (stage1 alone:
  128,423/196,608 bytes different, cosine 0.9974 -- not catastrophic, but not the established
  "cosine=1.000000, 0 mismatches" this project has repeatedly cited as the deployed baseline's own
  verified state). **Confirmed NOT a weight-hoist regression**: reloading the OLD, completely
  unmodified deployed baseline bitstream and rerunning the identical comparison reproduced the
  EXACT SAME mismatch counts, bit for bit (128423/14208/6808/1092/1618/1670, all six identical) --
  the two bitstreams agree with each other, both disagree with the reference. Root cause traced to
  `mac_array_ckpt_dump.cpp` (the tool that generates `ckpt_hw_*`): its own `mac_array_top()` call site
  was still using the interface from BEFORE the gmemmeta_elim1 register-map change (`&desc, 1, ...`,
  8 args) while `mac_array.h`'s current prototype needs a single by-value desc plus two `burst_maxi`
  params (9 args, `mac_array.h:542`) -- this tool has been silently broken (a compile error, not a
  silent wrong-answer) since that interface change and nobody had re-run it since, until this round
  needed it to regenerate two missing per-entry dumps (`entry_63.bin`/`entry_64.bin`, never captured
  before -- the tool's own dump range was hardcoded to only entries 0-16 and 74-80, see its `if (i <=
  16 || ...)` condition). Fixing the call site to compile also regenerated `ckpt_hw_*` as a side
  effect (same tool, same run) -- and the freshly-regenerated file doesn't match either bitstream.
  **`ckpt_hw_*` is untracked by git (`git status` shows `??`), so there is no way to recover
  whatever version was on disk before this session's regeneration to compare against.** Candidates,
  none yet checked: (a) the call-site fix itself has a bug (an `in_burst`/`out_burst` wiring
  difference from other testbenches' own working versions of this same call pattern); (b)
  `mac_array.cpp`'s own new-interface code path (added for signature consistency during the
  gmemmeta_elim1 round, per that round's own commit note) was never actually exercised end-to-end
  before this round -- possible latent bug from that round, only now surfaced; (c) something else.
  Deliberately NOT investigated further this round (board-testing weight-hoist was this round's own
  scope; this is a separate, real problem needing its own round). **Practical consequence: full-
  network checkpoint-level correctness cannot currently be verified for ANY build, weight-hoist or
  otherwise, until this is root-caused.**
  **CORRECTION, same day, follow-up round: "`entry_02/03.bin` remain trustworthy" above was WRONG --
  checked timestamps, not assumed. ALL of `entry_00.bin` through `entry_16.bin` (not just 63/64) got
  silently regenerated by the same `run_ckpt_dump.tcl` rerun (mtime 17:00-17:01, same run as the
  `ckpt_hw_*` regeneration) -- entry3's own board PASS this round was NOT independently validating
  against untouched data the way it looked like at the time.** The real independent cross-check turned
  out to be a different, better one: `accuracy_test_imgs_256/ckpt_ref_stage1_0000.npy` (the ONNX
  float32 reference used for the comparison script's `onnx_cos` column) is dated **2026-08-21**,
  genuinely untouched by anything in this session (it's Python-generated, `tools/
  gen_ckpt_reference_256.py`, never touched by the C++ `mac_array_ckpt_dump.cpp` fix). The real
  board's stage1-vs-ONNX cosine, both old baseline and new weight-hoist build, came back **0.6313** --
  matching, to 4 decimal places, the SAME figure this project has cited repeatedly across multiple
  much-earlier rounds (see the "0.6313" references elsewhere in this file, e.g. the per-channel-
  calibration round and the Stem-artifact round) as stage1's own established, real cosine-vs-ONNX
  state. **This decisively confirms the real board (both bitstreams) is computing the CORRECT result
  -- the freshly-regenerated `ckpt_hw_*` fixed-point reference is what's wrong, not the hardware.**
  General lesson: when hunting for an untouched reference to cross-validate against after a session
  touched a batch of generated files, checking mtimes on ALL of them (not just the ones you didn't
  personally edit a script for) is required -- a whole related FAMILY of files regenerated by the same
  tool run can look independently untouched at a glance while actually all being fresh from the same
  event; a genuinely-independent reference generated by a completely different tool/language (here,
  Python vs. this session's C++ edit) and dated well before the session is much stronger evidence.
  **Also found this round, confirming the user's own suspicion this is a systemic gap, not an isolated
  miss: scanning every real (non-generated) source file that calls `mac_array_top()` found 3 MORE
  files besides `mac_array_ckpt_dump.cpp` still on the pre-gmemmeta_elim1 signature (`&desc, 1, ...`),
  silently broken the same way since that interface change and never re-run since:
  `dw_linebuf_real_tile.cpp`, `verify_bundle_entry5_dw.cpp`, `writeout_edge_probe/
  writeout_edge_probe.cpp`.** This is the same failure class as this file's own "MacLayerDesc had only
  27 fields, missing the 28th" entry and the "shared header ripples into three testbenches" pattern --
  an interface change gets applied to the primary/deployed call sites and verified there, but
  secondary tools that share the same header don't get compiled (let alone run) as part of that
  round's own check, so they go silently stale until someone happens to need them. **Operational rule
  going forward: any change to `mac_array_top()`'s signature (or any other shared-header interface)
  should grep for every real call site (`grep -rln "mac_array_top(" --include=*.cpp .`, filtered to
  exclude `.autopilot`/generated build directories) and at least attempt to compile each one, not just
  the call sites the round's own primary work happens to touch.**
  **FOLLOW-UP, same day: a strong, plausible hypothesis for the root cause -- tested and REFUTED, not
  just proposed.** `git log` confirmed `tools/compare_board_full_network_ckpts.py` was created in
  commit `f7196f3` (2026-08-23), whose own message states the deployed DW mechanism THAT DAY was
  tile-based (`mac_array.cpp`) -- so `run_ckpt_dump.tcl` legitimately linking `mac_array.cpp` was
  correct when written. DW raster integration (`000350a`, 2026-08-28, 5-6 days later) replaced the
  DEPLOYED mechanism with `dw_raster_layer.cpp`, but `run_ckpt_dump.tcl` was never updated to match --
  a real, confirmed instance of the same interface/dependency-drift class as the bullet above, just on
  which IMPLEMENTATION file gets linked rather than which function signature gets called. **Fixed the
  link (now `mac_array_raster_integrated.cpp` + `dw_raster_layer.cpp`, matching every other current
  csynth/P&R tcl) and regenerated `ckpt_hw_*` -- the mismatch counts against the real board did NOT
  change AT ALL (128423/14208/6808/1092/1618/1670, identical to the wrong-implementation version, down
  to the exact digit).** This is decisive: two DIFFERENT DW implementations (tile vs. raster) produced
  the SAME (wrong, relative to real board) reference output from this testbench, which rules out
  "wrong implementation linked" as the (sole) root cause -- whatever's wrong is common to both, most
  likely inside `mac_array_ckpt_dump.cpp`'s own host-side orchestration (its 82-entry sequential loop
  sharing one persistent ping-pong `act_buf` across entries -- a pattern that a single-entry test like
  entry3's own board PASS cannot exercise or catch) rather than in either HLS implementation. **Not yet
  root-caused as of this entry** -- next step would be entry-by-entry bisection (run entries 0-4 only
  and check whether the divergence is already present), not attempted this round given diminishing
  returns after ruling out the most promising hypothesis. General lesson: when a plausible root-cause
  hypothesis is cheap to test (here: change one `add_files` line, rerun), test it and report the
  actual result even if it doesn't pan out -- a refuted hypothesis with a clean before/after
  comparison is real, useful progress (rules out a whole class of explanation), not a wasted round.
  **CLOSING UPDATE, same day: confirmed via git history that `ckpt_hw_*` was stale across at least
  THREE real deployed-architecture changes since its last legitimate generation, and this changes what
  every "checkpoint byte-exact" claim made since 2026-08-23 actually proves.** `run_ckpt_dump.tcl`/
  `mac_array_ckpt_dump.cpp` were last touched 2026-08-22/23 (commits `38e7001`/`da15315`), the exact
  same window `tools/compare_board_full_network_ckpts.py` was created in (`f7196f3`, whose own message
  confirms tile-based DW was the deployed mechanism that day -- the reference was genuinely correct
  when the tool was last run). Three real, substantive deployed-architecture changes followed with NO
  regeneration of this reference: the `PW_FLAT` II=2->1 fix (`32a6f1d`, 2026-08-27), DW raster
  integration (`000350a`, 2026-08-28), and gmem_meta elimination (2026-08-31, the current deployed
  baseline -- this is also the round whose interface change made the tool stop compiling entirely,
  per the entry above). **Every "N/N checkpoints byte-exact vs csim" claim made in this project after
  2026-08-23 -- including the deployed baseline's own README -- was either comparing against this same
  increasingly-stale reference without regenerating it, or (after 2026-08-31) could not have actually
  re-run this check at all, since the tool didn't compile.** Note this is a DIFFERENT, additional
  finding from the "wrong DW implementation linked" hypothesis refuted above -- that was about WHICH
  implementation generates the reference; this is about HOW LONG it's been since it was regenerated at
  all, regardless of implementation. **Practical resolution: rather than keep chasing the exact bug in
  `mac_array_ckpt_dump.cpp`'s own orchestration (entry-by-entry bisection would be the next step, not
  attempted -- diminishing returns after two rounds of hypothesis-testing), the weight-hoist round was
  closed out using `ckpt_ref_*_0000.npy` (the ONNX float32 reference, `tools/gen_ckpt_reference_256.py`,
  dated 2026-08-21, genuinely untouched and independent of any C++/HLS toolchain this project uses) as
  the primary correctness judge instead: the weight-hoist bitstream's real board run matched the
  established figures across all six checkpoints EXACTLY (0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811,
  identical to the old baseline's own re-measurement) -- a clean, decisive, independently-verified
  correctness confirmation, without needing `ckpt_hw_*` fixed at all.**
  **OPERATIONAL RULE GOING FORWARD, applies beyond this one round: report checkpoint correctness
  primarily as cosine similarity against `ckpt_ref_*.npy` (the ONNX float reference), not byte-exact
  count against `ckpt_hw_*` (the csim fixed-point reference) -- the ONNX reference is the one this
  project has now twice confirmed to be trustworthy and stable across architecture changes (Python-
  generated, no dependency on whichever C++ implementation happens to be currently linked into a
  quickly-bit-rotting testbench), while `ckpt_hw_*` has now been shown capable of silently drifting
  out of sync for weeks across multiple real architecture changes with nobody noticing. Demote
  `ckpt_hw_*`/byte-exact-vs-csim to a secondary "did this specific round regress vs. the immediately
  prior round" signal (still useful for that narrower question, e.g. the same-day A/B test that first
  proved weight-hoist wasn't the cause of the mismatch), not a standalone correctness claim. This is a
  process change for every future board round on this project, not just this one.** `ckpt_hw_*`'s own
  underlying bug (why even a freshly-relinked regeneration still doesn't match real board output) is
  left genuinely unresolved -- flagged for whoever next needs byte-exact csim-vs-board checking badly
  enough to justify the entry-by-entry bisection this round didn't do.
- **RESOLVED 2026-08-30 (was open earlier the same day): `mac_array_tb.cpp` (the legacy 17-phase csim
  suite) fails most DW-conv phases and SIGSEGVs when run against `mac_array_raster_integrated.cpp` --
  this is a testbench/source pairing mismatch AND a real, confirmed raster-DW bug, but the bug's real
  shape range does not overlap the real network's, so the deployed 3,608.76ms board number is NOT
  affected.** First found 2026-08-29 (ZHR-92, incidentally during
  gmem_meta-elimination csim); the same day's follow-up round fully explains it, correcting an
  over-strong causal claim made in the first pass (retracted: "board-validated but never csim
  re-checked" -- wrong, see below). Confirmed via three fresh, clean-project csim runs: (1)
  `mac_array_tb.cpp` + the ORIGINAL `mac_array.cpp` (not the raster file) passes all 17 phases
  (Phase0-Phase16), 0 errors -- this IS the real, reproducible "17/17" (also resolves an apparent
  16/16-vs-17/17 discrepancy: Phase0 was added after the commits citing "16/16"); (2)
  `mac_array_raster_integrated.cpp` (added fresh in commit `000350a`, 1601 new lines, never a
  modification of `mac_array.cpp`) came with its OWN separate, purpose-built testbench,
  `mac_array_raster_integrated_wiring_tb.cpp` (added in the same commit) -- passes 4/4, 0 mismatches,
  on 4 real FastViT-T8 layer shapes; `000350a`'s own commit message never mentions "csim" at all, only
  board timing and P&R WNS, consistent with the wiring tb (not the legacy suite) being that round's
  real acceptance gate. `run_csim_integrated_full.tcl` (also added in `000350a`, wiring the OLD
  `mac_array_tb.cpp` to the NEW raster file) exists in the repo but nothing suggests it was ever
  actually run as an acceptance check before this investigation -- this round was the first time
  anyone ran it. **This is "we have been testing two different configurations without realizing it,"
  not a code regression** -- matches this project's own "check what a claimed validation actually
  consumed" lesson class, just discovered about a testbench/source pairing instead of a data artifact.
  **What IS real, not just a pairing mismatch, CONFIRMED 2026-08-30 via per-element mismatch-location
  diagnostics on Phase3/Phase4 (added temporarily, removed after use): this is a genuine bug in the
  raster DW mechanism, not a legal testbench-pairing difference.** Every phase that fails against the
  raster file touches the DW path; the SE-block phase (no DW/PW) is the only passing real-compute
  phase, matching the raster integration's own claim that PW is untouched. Decoding Phase3 (DW:
  cin=11, cout=11, h_in=w_in=15, k=3, s=1) and Phase4 (DW: cin=9, cout=9, h_in=w_in=17, k=3, s=2)
  mismatches to (channel, row, col): failures are NOT concentrated at tile edges/remainder tiles (row
  0/col 0, no tile boundary involved, fails at the same ~95%+ rate as every other position) -- ruling
  out "legal difference in undefined/boundary handling." Every mismatch shows the actual output as
  exactly 0 against varied golden values, and the total mismatch count exactly equals the DW-region-
  only mismatch count (no stray writes elsewhere) -- the signature looks like "this region was
  essentially never written," not "computed with a different but valid convention," and is identical
  whether DW is dispatched first (Phase4) or second after a PW call on the same shared buffer (Phase3),
  ruling out inter-call state leakage. What HAS been validated (the wiring tb's 4/4 real-shape cases)
  all use cin=48 or 96 with h_in/w_in exact multiples of MAC_PR/MAC_PC=4 (64, 128) -- Phase3/4's small
  cin (9, 11) and non-multiple-of-4 spatial dims (15, 17) are a real, qualitative difference from what
  was tested. fpg is NOT the distinguishing factor here (Phase3/4 both use fpg=1, same as most real
  layers) -- this is a SEPARATE issue from the fpg=48/SIGSEGV one below.

  **Decisive check, 2026-08-30, via `tools/layer_descriptor_256.json` directly (not assumed from a
  divisibility analysis done for a different purpose -- ZHR-64's own MAC_PR/PC divisibility check was
  explicitly NOT reused here since it doesn't guarantee this condition): all 25 real `dwconv` layers
  have cin in {48,96,192,384} (all >=48) and h_in=w_in in {128,64,32,16,8} (all exact multiples of 4).**
  Also confirmed layer 0 (Stem, `op_type=conv`, cin=3, h_in=w_in=256 -- the one real layer with small
  cin) is never dispatched through `mac_array_top` at all; its output is precomputed on the ARM
  (`stem_output_0000.bin`, loaded as a file) and never touches the raster DW mechanism. **The real
  82-entry dispatch sequence never enters the raster DW bug's failure regime -- the deployed
  3,608.76ms number is not built on wrong DW computation.**

  Trigger-condition isolation (two synthetic single-DW-layer tests, added temporarily and removed
  after use): **cin=8 with h_in=w_in=16 (exact multiple of 4) FAILS** (1621/4096 mismatches) -- small
  cin alone breaks it even with safe spatial dims. **cin=48 (matches every real layer) with
  h_in=w_in=15 (NOT a multiple of 4) CRASHES** outright (worse than any Phase3/4 result, which
  combined small cin AND non-multiple dims and only produced wrong values, not a crash). **Both
  factors independently trigger a failure — not just one.**

  **CLOSED 2026-08-30: assertion added, not a fix (as directed -- costs nothing on the real network,
  guarding rather than reworking the small-cin channel-chunking and non-multiple-tile boundary
  handling the real fix would require).** `mac_array.h`'s `mac_check_supported_shape()` (right after
  `LayerDescV2`'s own definition) consolidates this DW cin/dims check alongside the fpg<=2 and K<=
  MAX_K=7 checks, called once at `run_layer`'s entry in `mac_array_raster_integrated.cpp` (NOT called
  from `mac_array.cpp`'s own `run_layer` -- that mechanism doesn't have these limits, confirmed by its
  own csim suite passing every one of these exact shapes). `DW_CIN_MIN_SAFE=48` is explicitly
  documented as the empirical floor (cin=8 confirmed fails, cin=48 confirmed passes, nothing in
  between tested) not a theoretical one. Fires as plain `assert()` guarded by `#ifndef
  __SYNTHESIS__` -- the standard Xilinx idiom: present and firing in csim, entirely absent from
  synthesized RTL, zero real hardware cost, cannot affect any deployed bitstream. Verified against
  all three real testbenches with zero false positives: `mac_array_raster_integrated_wiring_tb.cpp`
  4/4, `dw_raster_layer_tb.cpp` 5/5 (the file's own "csim-verified 5/5" claim, now directly
  reproduced, not just cited), and `mac_array.cpp`+`mac_array_tb.cpp` 17/17 (unaffected, as expected).
  Confirmed to fire correctly and loudly on the very first out-of-range real test (`mac_array_tb.cpp`
  run against the raster file aborts at Phase0 with an explicit, named assertion message, not a
  silent wrong answer) -- this is the intended behavior now, not a new failure.

  This is the third confirmed instance in this project of "a compile-time/architectural assumption
  silently narrower than a runtime value's real range, invisible because the real network's own
  shapes happen to avoid it" (after fpg=2 and MAX_K=7) -- worth treating as a standing category to
  check for in any new HLS mechanism, not just
  reacting to each instance separately.

  SIGSEGV specifically localized to Phase11 (fpg=48 synthetic stress test, h_in=w_in=256 -- NOT a real
  network shape; real FastViT-T8 only uses fpg in {1,2}) -- concrete contributing factor found via
  source read: `dw_raster_layer.cpp`'s active consume path hard-bounds every fpg-indexed local array
  and its own reduction loop at `DWR_MAX_FPG=2` regardless of runtime `fpg`, so fpg=48 structurally
  only computes 2 of 48 channels -- **the strongest candidate, not confirmed via an actual backtrace**
  (no debugger available in this environment). Zero real-network impact even if fully confirmed
  (real fpg never exceeds 2), but the hard bound has no overflow guard, a latent hazard if fpg's real
  range ever changes.
- **Accuracy (Phase 0.7, ZHR-8, 9 rounds deep, budgeted 2 days / now on day 3 — final round in
  progress)**: end-to-end cosine similarity (board vs ONNX float32 reference) is currently 0.0519,
  down from an earlier-measured 0.4788 — but that earlier number is now known to be invalid, not a
  better baseline: it was measured while the ARM was reading a stale cache line instead of FinalDW's
  real output (see the cache-coherence constraint above), so 0.0519 is this project's first
  cache-correct accuracy measurement, not a regression from a trustworthy one. The sigmoid LUT
  indexing bug in `se_block()` (`(uint8_t)ex[co]` should be `(uint8_t)(ex[co]+128)`) is real and
  fixed (commit ab8cff4) but confirmed NOT the main driver of the gap. Root cause of the remaining
  gap is NOT yet localized to a specific layer or mechanism — do not assume it's an SE-block
  quantization issue (layer 50/51 `out_shift`) without first checking whether the ONNX
  reference-point itself is correctly aligned to what the driver computes (an untouched-since-Phase-0.7-
  start suspicion: the driver may never call a final GELU that the ONNX graph applies) and running a
  real hardware-vs-ONNX per-stage cosine breakdown — this project has never done a layer-by-layer
  hardware-vs-float comparison, only hardware-vs-hardware and aggregate end-to-end numbers. See ZHR-8
  for the full round-by-round chain and ZHR-63 for the current exit criteria.
- **FinalDW's own IP computation**: confirmed correct (ZHR-8 step 6, single-variable cache test) —
  the earlier "collapses to hard zero" symptom (ZHR-8 Phase 0.6) was the same ARM-side stale-cache-read
  issue above, not a hardware/HLS correctness bug in the IP itself.
- **git**: this repo had no version control until 2026-08-13. The initial commit is a single
  consolidated snapshot (no prior VCS existed to replay), with annotated tags pointing at real
  preserved historical source (`fastvit_ip_v1.2_backup/`, `dwconv_worker.tile_backup_2530ns.cpp`,
  etc.) — see `git tag -l -n99` for a navigable timeline and each tag's message for what it actually
  represents.
