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
  **RE-OPENED, 2026-09-04 (ZHR-92): the closure's own premise ("`gmem_w`'s single AXI port can't
  service more than one bus request per pipeline iteration") was measured on the PRE-weight-hoist
  architecture, where `PW_FLAT` read every weight from `gmem_w` on every call, always -- that demand
  is now largely gone (`pw_weight_cache`, `PW_WCHUNK`, deployed since 2026-09-02/03, see the baseline
  section above). Re-checked per this file's own "an `#ifdef`-disabled variant's historical numbers
  must be re-measured, never re-cited" rule (see the DW FPG-specialization entry elsewhere in this
  file for why this matters): flipped `MAC_PD` 1->2 on the CURRENT (cached) architecture, isolated
  csynth. Result: `PW_FLAT` still regresses to achieved II=2, and the diagnostic still names `gmem_w`
  by name (`Unable to schedule bus request operation ('gmem_w_load_1_req'...) due to limited memory
  ports`) -- textually the same port as the original closure. **But a follow-up confirmatory test
  shows this is NOT the same mechanism in substance.** `pw_cached` is a plain runtime bool (deliberately
  NOT templated, per this function's own header comment: "only one arm touches gmem_w... unlike
  FAST_WRITEOUT's own same-port-two-writes trigger" -- true when MAC_PD=1, since the per-`dd` loop
  had only 1 iteration). At MAC_PD=2 the `for(dd<MAC_PD) UNROLL` loop now duplicates the ENTIRE
  `if(pw_cached){BRAM}else{gmem_w}` branch across 2 simultaneous lanes -- HLS must conservatively
  schedule for both lanes hitting the dead `gmem_w` arm in the same cycle, even though `pw_cached` is
  always true on every real dispatched shape (PW_WCHUNK caches 100% of real PW layers). Confirmed via
  a direct test (`PW_FORCE_CACHED_ONLY_TEST`, a compile-time-constant `if(true)` that lets HLS
  dead-strip the uncached arm entirely): **`PW_FLAT` recovers to achieved II=1 at MAC_PD=2 with the
  dead branch removed from the scheduler's view -- no `gmem_w` II violation for `PW_FLAT` at all.**
  This is a 7th confirmed instance of this file's own "a runtime value gating entry to a critical
  hardware region costs real hardware every time" principle, on a new trigger: a runtime branch
  previously judged safe under an implicit "only one arm ever touches the contended resource per
  iteration" assumption that was true only at the specific unroll width (MAC_PD=1) it was written
  under, and silently stopped holding once that width changed, with nobody re-checking the assumption
  against the new value. **Practical consequence: MAC_PD=2's II=2 is a scheduler artifact from
  dead-but-still-scheduled code, not a real bandwidth wall the way it was pre-weight-hoist -- the
  original closure's stated mechanism no longer applies to the current architecture.** Whether to
  actually strip the dead uncached arm for real (vs. leaving `PW_FORCE_CACHED_ONLY_TEST` as a
  diagnostic-only flag) and proceed to resource/P&R/board measurement at MAC_PD=2 is a decision point,
  not yet made -- this entry records the diagnostic result only, per this project's own one-round
  discipline.
  **CLOSED OUT AS A REAL WIN, same day, later round: the dead arm was made compile-time-eliminated by
  default (not just a diagnostic flag -- `PW_ALLOW_UNCACHED_FALLBACK` now gates the OLD runtime-gated
  behavior, off by default, matching this project's own dead-but-kept-fallback convention), and the
  full judgment sequence (isolated csynth II=1 confirmed on real source, real P&R, real board) passed
  end to end. Real P&R closed on the FIRST attempt (WNS=+0.094463ns, no shared-multiplier-style chase
  needed, unlike PW_WCHUNK's own 3-round saga) -- LUT 35,751/53,200=67.20% (+5.80pp over the
  MAC_PD=1 baseline), BRAM 82/140=58.57% (+8 tiles), DSP 50/220=22.73% (unchanged). Board: full
  network 1,806.45ms->**1,626.70ms (-9.95%)**, byte-exact, ONNX cosine EXACT match
  (0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811). PW-only (entry3) improved -14.1% to -18.7% (mean
  ~15.7%), matching the pre-registered model (PW's 28% compute-time share, from the cosim
  decomposition, doubling in parallelism predicts ~14% of PW's own time) closely once properly scoped
  to a PW-only shape -- the model held. DW-only (entry5_dw) unaffected (38.82ms->38.87ms, within
  noise), confirming the win is specific to PW's `gmem_w`-adjacent mechanism, not a general MAC_PD
  effect. **`mac_array_a3_macpd2` is now the deployed baseline** -- see the baseline section below for
  the full writeup. BRAM dual-port question (would MAC_PD=4 need `pw_weight_cache` partitioning) was
  never reached -- `gmem_w`'s dead-branch artifact was the binding constraint at MAC_PD=2, not BRAM
  ports; MAC_PD=4 remains untested. 150MHz was deliberately deferred to its own separate round to
  avoid confounding two simultaneous architectural changes.**
  **MAC_PD=4, same day, next round: the BRAM dual-port question resolved itself -- HLS auto-inferred
  the exact partitioning needed, no manual pragma required.** `pw_weight_cache` was (and remains)
  completely unpartitioned in source; at MAC_PD=4 HLS's own pipeline-scheduling pass emitted `[HLS
  214-270] Inferring pragma 'array_partition type=cyclic factor=2 dim=1'... due to pipeline pragma`
  and applied it automatically -- 2 auto-inferred banks x 2 native BRAM read ports/bank = 4
  simultaneous reads, exactly covering MAC_PD=4's need, at HALF the naive factor (4) a hand-written
  partition might have reached for. `PW_FLAT` achieved II=1 immediately, zero diagnostic violation.
  Isolated csynth showed a real stop-condition trigger (LUT 56,358->70,661, +25.4%, projecting via
  MAC_PD=2's own measured real/isolated ratio to ~84.3% real -- above every prior successful closure
  on this line) -- reported per this round's own pre-registered "stop if LUT jumps a lot" rule rather
  than running P&R automatically. Decision: ran real P&R anyway, on the explicit reasoning that
  MAC_PD=1->2's own isolated-to-real divergence went the FAVORABLE direction and this project's own
  history says the direction isn't predictable in advance. **Real P&R came back the best-margin build
  on this session's entire timing history despite the highest LUT occupancy**: WNS=+0.338785ns (LUT
  80.79%, the isolated projection overshot -- real came in lower, but still the highest occupancy this
  project has ever closed timing at) -- see the "utilization percentage doesn't predict WNS direction"
  finding this specific result produced, now recorded as its own lesson below. Board: full network
  1,626.70ms->**1,522.39ms (-6.41% further, -15.73% cumulative from MAC_PD=1)**, byte-exact, ONNX
  cosine exact match at every MAC_PD value tested. PW-only improved a further ~9.4% (matching the
  pre-registered ~7-8% diminishing-returns model); DW-only unaffected at every step (1/2/4), confirming
  the whole line is PW-specific. **`mac_array_a3_macpd4` is now the deployed baseline.**
  **MAC_PD=8 is pre-registered as very unlikely to be worth attempting**: the diminishing-returns
  pattern (1->2 saved ~15.7% of PW's time, 2->4 saved a further ~9.4%, roughly halving each step,
  matching the "PW's compute share keeps halving" model) predicts only ~4-5% further PW-time savings
  from 4->8, for a LUT cost that -- by the same roughly-doubling growth pattern seen 2->4 -- could push
  real occupancy well past 90%, a region this project has never closed timing in. Not tested; recorded
  as a judgment call so a future round doesn't have to re-derive it.**
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
  **PRECISE FOLLOW-UP, 2026-09-02, same day: confirmed the real BRAM delta is single-copy, not
  double, which corrects the "ANY array parameter read from 2+ instantiations gets a physical copy
  per instantiation" rule stated above -- that rule is accurate at the ISOLATED CSYNTH level (already
  confirmed there, +128 BRAM_18K for the abandoned template-`PW_CACHED` design) but does NOT reliably
  hold at the REAL SYNTHESIS level.** 144KB's theoretical minimum is exactly 32 tiles
  (147,456 bytes / 4,608 bytes-per-RAMB36E1); the real measured delta is 40 tiles (74-34) -- a 1.25x
  packing/routing overhead, nowhere near the 2x (64 tiles) a genuine duplicate would show. Real
  Vivado `synth_design` evidently CAN share (or at least avoid duplicating) a single logical BRAM
  array across 2 HLS-level function instances that each treat it as their own parameter, even though
  HLS's own isolated csynth reports them as needing separate physical copies. **Corrected rule: an
  array parameter read from 2+ independently-synthesized function instantiations is GUARANTEED to
  show as duplicated in isolated csynth, but real whole-IP synthesis may or may not actually
  duplicate it in the final implementation -- this is now confirmed to differ (isolated says "yes,
  duplicated," real P&R says "no, effectively single-copy") for at least one real case on this
  project, so treat isolated csynth's verdict on THIS specific question the same way every other
  isolated-csynth number on this project is already treated: a hypothesis to test with real P&R, not
  a fact.** This is the same "isolated vs real, direction not consistent" pattern as the BRAM
  percentage finding directly above, just narrowed to a more specific mechanism (array-parameter
  sharing across instances) rather than the aggregate resource total.
  **FOLLOW-UP ATTEMPT, same day, CLOSED with a real negative result: raising the cutoff to 432KB
  (covering all 26 real PW layers, eliminating the fallback path entirely) does NOT fit.** Motivated
  by real board evidence first (not a guess): the 4 layers that fall back to direct-DRAM-read at the
  144KB cutoff (layer 43/44/47/48) were board-measured, via the SAME `PW_FIX_WADDR` probe + two-
  implementation cross-validation technique described above (confirmed a 2nd time on this exact
  measurement -- see that entry), saving 351.44ms combined (78.7% of their own 446.42ms, a
  remarkably consistent 77.9-79.3% across all four) if cached -- 16.65% of the full network. This
  REFUTES an unchecked intuition that these much-larger-cin/cout layers would be more compute-bound
  (and therefore have a SMALLER weight-time fraction than the small entry3-scale layers the cache
  was originally sized around) -- measured, their weight-time fraction (78.7%) is HIGHER than
  entry3's own 55.3%, not lower, because these are small-spatial-extent (h=w=8, 4 tiles)/large-
  channel-count layers where weight volume (scales with cin*cout) dominates more, not less, than
  compute per tile. Given this real, substantial, measured opportunity, raising
  `PW_WEIGHT_CACHE_ELEMS` to 442,368 (432KB, the real max across all 26 layers) was tried: isolated
  csynth projected 115% BRAM (323/280 BRAM_18K) -- below this round's own pre-registered 130%
  "definitely won't fit" cutoff, so real P&R was run anyway (per the same "isolated is unreliable,
  direction not consistent" discipline already established) rather than trusting the isolated number
  either way. **Real P&R: Block RAM Tile 140/140 (100.00%, the device's absolute ceiling, zero
  margin) and WNS=-1.075730ns (a real violation, not closed).** Both land squarely in this project's
  own pre-registered stop-loss zone -- reverted immediately (`PW_WEIGHT_CACHE_ELEMS` back to 147,456/
  144KB, the real deployed, closed-timing configuration) without attempting a pblock or other tuning
  fix, per this file's own hard-stop-list precedent on pblock-rescue attempts for this class of
  signature. **The 351.44ms opportunity is real and not yet captured** -- the pre-registered next
  candidate is chunked loading (same 144KB buffer, the 4 big layers load their weight in <=144KB
  pieces reusing the cache across chunks, BRAM-neutral by construction) but needs its own new
  outer-chunk loop plus a fresh re-verification of "activation read costs ~0%" (board-confirmed only
  under the current non-chunked design; chunking would make each of these 4 layers' `COPY_FROM_ROW`
  re-run 3x, an assumption never tested). Not attempted this round.
  **PREREQUISITE PROBE DONE, same day (2026-09-02), before writing the chunked loop as instructed:**
  `PW_FIX_ACTADDR` on the same 4 entries (WNS=+0.129376ns, clean, no cross-validation needed) measured
  current activation-read contribution at **1.32ms combined / 446.42ms baseline (0.30%)** -- confirms
  the "~0%" finding transfers to these small-spatial/large-channel layers, not assumed from entry3.
  Extrapolating x3 (chunking triples `COPY_FROM_ROW`'s re-runs, explicitly flagged as extrapolation not
  measurement) adds ~2.64ms. Chunked-load overhead, via the established `PW_WEIGHT_HOIST` cost model
  (~1.475ms per 144KB chunk at II=1/100MHz, proportional to bytes moved): L43/L44 each need exactly 3
  chunks (442,368B = 3x144KB) = 4.425ms each; L47/L48 each need 2.5 chunks' worth of bytes (368,640B) =
  3.6875ms each; **total load overhead = 16.225ms** (currently zero -- the direct-read fallback has no
  separate load phase). **Net benefit = 351.44 − 2.64 − 16.225 ≈ 332.6ms**, clearing this round's own
  pre-registered >200ms "write the chunked loop" threshold by a wide margin. Chunked loading is now the
  pre-registered next implementation target -- not yet attempted, awaiting a checkpoint per this
  project's own one-round-at-a-time discipline (see the entry directly below).
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
- **The same resource/port NAME in an HLS diagnostic can mean two completely different mechanisms at
  different points in this project's history — citing a historical closure by the diagnostic's named
  resource alone, without checking the diagnostic's own surrounding condition, is not the same claim
  as confirming the mechanism still applies.** Confirmed 2026-09-04 (ZHR-92, MAC_PD-expansion
  re-open): the 2026-08-31 closure of MAC_PD-expansion cited `gmem_w`'s single AXI port as the reason
  `PW_FLAT`'s achieved II regressed 1->2 at MAC_PD=2 — real, load-bearing weight-read bandwidth demand
  at the time. Weight caching (`pw_weight_cache`/`PW_WCHUNK`) later removed that real demand entirely
  — `PW_FLAT`'s hot path no longer touches `gmem_w` at all on any real layer. Re-testing MAC_PD=2 on
  the current architecture still showed achieved II=2, and the diagnostic still named `gmem_w` by
  name, verbatim — which would read, at a glance, as "the same closure reason, still valid, don't
  bother re-opening this." It wasn't: a follow-up test (forcing the dead uncached fallback arm to a
  compile-time constant so HLS could dead-strip it) recovered II=1 immediately, proving the real
  mechanism was a scheduler artifact from MAC_PD's own unroll duplicating a dead branch across lanes
  — completely unrelated to `gmem_w`'s actual bandwidth, which was never the constraint on the current
  architecture at all. Confirmed by dead-code elimination, not inferred from the diagnostic text.
  **This is a distinct variant of this file's own "a claimed test result is not evidence unless
  reproduced" rule (see the fabricated-numbers entry and the `csim-verified N/N` staleness entries
  elsewhere in this file): there, the risk was trusting a claim without re-running it; here, the risk
  is trusting a NAMED MECHANISM (a port, a resource, a diagnostic string) without checking whether the
  surrounding conditions that made it load-bearing are still true.** A resource name recurring in an
  HLS diagnostic is not proof the same causal story still applies — the diagnostic only says "this
  operation couldn't be scheduled onto this resource under the current constraints," and what changed
  around it (an architecture change eliminating the resource's real demand, leaving only a dead
  branch's structural footprint) is invisible from the diagnostic text alone. Before re-citing a
  historical closure whose stated reason names a specific port/resource, check what ELSE has changed
  in the surrounding code since that closure was written — not just whether the same diagnostic string
  reappears.
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
- **PW's cost decomposition must be re-measured from scratch after ANY architectural change to the
  component that used to dominate it -- an old percentage split does not survive the mechanism it was
  measured against being replaced, even directionally.** Confirmed 2026-09-03 (ZHR-92, "open B" round,
  post-chunking): the pre-chunking decomposition (weight-read 55.3%, activation ~0%, output ~0%, 27%
  unexplained, measured on the 3,630.74ms baseline) was NOT reused or extrapolated -- redone with 4
  real-P&R fixed-address probes (weight now BRAM-cached, activation, output, and a NEW bias/shift-cache
  probe never run before) on the CURRENT chunked-weight deployed baseline, on both a small (entry3) and
  large (entry64) layer. **Result: all 4 combined explain ~0% of PW's time** (every probe's delta
  within noise, <=0.06ms, of baseline) -- weight-read's old 55.3% share is gone (confirmed, not
  assumed: BRAM-cached reads have uniform latency regardless of address, so fixing the address changes
  nothing measurable), and none of the other three ever mattered. A trip-count-based pipeline-only
  estimate (`total_iters x II(=1) x 10ns`, matching this file's own established `Performance Estimates`
  discipline) plus the separately-established ~0.58ms/entry dispatch floor together explain only 46-57%
  of measured time (entry3: 9.83ms pipeline + 0.58ms dispatch = 45.9% of 22.67ms; entry64: 18.43ms +
  0.58ms = 56.8% of 33.46ms) -- **a large remainder (43-54%) unaccounted by anything tested.** New gap
  in this project's own probe coverage identified while investigating: `PW_FIX_ACTADDR` only ever fixed
  `COPY_FROM_ROW`'s SRAM-to-SRAM copy, never `ROW_READ`'s own real DRAM burst fetch (`in_burst.
  read_request`/`read()`, genuinely data-dependent address, once per row-tile x Cin channels) -- the
  "activation read ~0%" finding this project has cited repeatedly only ever covered the on-chip copy,
  not the DRAM fetch feeding it. Flagged as the strongest untested candidate for the remainder, not
  measured yet -- do not assume it explains the gap until a dedicated `ROW_READ`-address probe actually
  runs; this file's own repeated lesson on this exact line (weight-read's 1.3% vs 55.3% reversal, see
  above) is precisely "measure the CURRENT mechanism, don't extrapolate from a prior one's number."
  **FOLLOW-UP, 2026-09-04, same round: the "strongest untested candidate" was tested and ALSO came back
  ~0%.** `PW_FIX_ROWREAD_ADDR` fixes only `ROW_READ`'s `read_request` address argument (constant,
  `d.in_off>>2`) while leaving `n_words` (burst length) computed from the real per-`(rr,ci)` address
  exactly as before -- isolating address locality from burst-length effects, the same discipline
  `PW_FIX_WADDR` used for the (successful) weight-read probe. Real P&R closed (WNS=+0.288714ns).
  Board: entry3 22.67ms->22.67ms (Delta 0.00ms), entry64 33.46ms->33.44ms (Delta -0.02ms) -- both
  noise, matching all 4 prior probes exactly. Mismatch counts (147517/196608, 56197/73728) confirm
  genuine engagement, not a no-op. **Five real-P&R-confirmed probes now (weight-cache, activation
  SRAM copy, output write, bias/shift cache, ROW_READ DRAM address), ALL ~0% -- the candidate list is
  exhausted, not merely thin.** None of PW's still-large remainder (54.1%/entry3, 43.2%/entry64) is
  attributable to any addressing site tested so far. Practical conclusion for whoever continues this
  line: stop reaching for fixed-address probes on this specific question -- the next diagnostic step
  is a full per-entry timeline (dispatch handshake, each named HLS region's own real elapsed
  contribution: `ROW_READ`'s burst COUNT/latency as opposed to address locality, `PW_WEIGHT_HOIST`/
  `PW_BIAS_HOIST`/`PW_SHIFT_HOIST`'s own real once-per-layer costs, `PW_FLAT`'s own fill/drain and any
  real stalling beyond the trip-count-only estimate), not another address-fixing variant.
  **RESOLVED, 2026-09-04 (several rounds later, after directly measuring transaction counts): the "5
  probes, all ~0%" result above is NOT actually a contradiction with the later finding that output-
  write TRANSACTION COUNT is a real, large lever -- it's exactly consistent, once the scope of what a
  fixed-address probe can measure is stated precisely.** A fixed-address probe (`PW_FIX_WADDR`/
  `_ACTADDR`/`_OUTADDR`/`_BIASADDR`/`_ROWREAD_ADDR`) tests "does WHERE we access cost anything" -- it
  forces the address to a constant, holding the NUMBER of accesses and their SIZE completely
  unchanged. It structurally cannot detect a cost that scales with access COUNT (too many small
  transactions) rather than access LOCATION (cache miss, DRAM row-buffer thrash, alignment penalty).
  The one probe in this whole line that DID show a large effect -- `PW_FIX_WADDR` on the pre-chunking
  weight-read path -- worked BECAUSE fixing the address collapsed 341x redundant re-reads into
  repeated hits on the SAME location (a location-cost effect, not a count effect); output write's own
  fixed-address probe stayed flat because fixing the address does nothing to the fact that it's still
  issuing the SAME NUMBER of tiny 4-byte writes. **General rule for this project going forward: a
  fixed-address probe answers "is this address-dependent," not "is this access-pattern-dependent" --
  a null result rules out the FORMER only. Before concluding a mechanism doesn't matter from an
  all-~0%-probes result, separately ask whether the real cost could be in access COUNT or GRANULARITY
  instead of access LOCATION, and check that with a transaction-count calculation (real code, real
  formula) or a scaling experiment, not another address-fixing variant.**
  **FOLLOW-UP, 2026-09-04, next round: no fine-grained on-board timer exists to measure named HLS
  regions directly, so the "full per-entry timeline" plan above was replaced with a scaling
  experiment instead** -- the same technique this project has used 3 times before (n_cbase
  decomposition, n_steps scaling, tile-count scaling), all previously effective. 11 synthetic PW
  bundles (trivial golden: all-ones input/weight, zero bias, `out_shift=7` -> `out=cin>>7`, no
  simulator needed), one dimension varied at a time (`n_ot`/cout in {48,96,192}, `n_cbase`/cin in
  {1,2,4} via cin in {32,64,128}, spatial tile count in {16,64,256}, `n_chunks` in {1,2,3} via cin
  in {192,576,960} at fixed cout=384/h=w=8). **Caught and fixed a real bug in the CSIM TESTBENCH
  during this round (not in the deployed design)**: an initial version shared one buffer for both
  `in_base`/`out_base` (mirroring `pw_weight_hoist_tb.cpp`'s own convention, with `out_off` offset
  past the input) -- but the REAL board driver (`mac_array_single_op_test.c`) uses two SEPARATE
  fixed DRAM regions (`in_v`/`out_v`), so these bundles' own `in_off=out_off=0` (matching the board
  convention, correct) aliased when tested through a shared-buffer csim harness, producing 4/11
  FALSE FAILURES that had nothing to do with the hardware -- fixed by giving the csim testbench
  separate `in_buf`/`out_buf` vectors, matching the board's own layout exactly; all 11/11 passed
  after the fix. All 22 board runs (11 points x2 repeat) came back byte-exact, repeatability
  0.01-0.06ms.
  **Finding: comparing each point's measured ms against the KNOWN ANALYTICAL PW_FLAT-only compute
  formula (`n_ot*(32*n_cbase+16)*n_tiles*n_chunks*10ns`, not a fitted term) shows the remainder is
  48.7%-71.9% of measured time across all 11 shapes (mean 57.8%, stdev 6.8pp) -- NOT a small fixed
  additive cost and NOT concentrated in any one of the 4 dimensions.** A literal additive fit
  (`ms = a + b*n_ot + c*n_cbase + d*n_tiles + e*n_chunks`, as the round's own pre-registered method
  asked for) came back with a poor R^2 (0.635 on raw ms, 0.693 on the remainder) and physically
  implausible coefficients (negative n_cbase term, huge n_chunks term) -- traced to two causes, both
  worth remembering for any future regression on this codebase's own timing data: (1) `n_cbase` and
  `n_chunks` are collinear within the n_chunks sweep group (both increase together, 6/18/30 vs
  1/2/3, since both derive from the same varying `cin`), and (2) the TRUE cost structure is
  multiplicative in these dimensions (matching PW_FLAT's own iteration-count formula exactly), not
  additive -- a linear-in-raw-features model cannot represent a multiplicative relationship well
  across an 8x range in n_ot and a 64x range in n_tiles. **Comparing against the known analytical
  formula instead of fitting a blind linear model to raw dimensions was the more informative move
  once the naive fit's R^2 came back poor -- don't force a linear fit to explain a relationship whose
  true functional form is already known from the source code.**
  **Working hypothesis for the remainder, NOT yet tested**: proportional (not fixed-additive) scaling
  with compute volume is consistent with a per-AXI-transaction handshake overhead (real cycles per
  `read_request`/`write_request`+`write`+`write_response` beyond the raw data-transfer cycles the
  trip-count formula assumes) -- this would scale with transaction COUNT, not address value, which is
  a DIFFERENT question from all 5 already-tested address-fixing probes (none of which isolated
  per-transaction handshake cost independent of the address used). Flagged, not measured.
  **RULED OUT, 2026-09-04, same round (no board time needed -- computed against the actual csynth
  report): a competing hypothesis, also proportional-with-compute in shape, was PW_FLAT's own
  pipeline fill/drain restarting once per `(rt,colt)` call (the flat trip counter covers one call's
  own steady state, so a call boundary restarts the pipeline -- and call COUNT scales with
  `n_tiles*n_chunks`, the same axis the remainder tracks).** `pw_flat_pipeline_impl_true_Pipeline_
  PW_FLAT`'s own csynth report (`PipelineII=1`, `PipelineDepth=22`) gives a real, not estimated,
  fill/drain cost of `(22-1)*10ns=210ns` per call. Computed across all 13 shapes measured so far
  (the 11-point scaling sweep plus entry3/entry64): **worst case is 0.4% of the remainder** (256
  calls, the highest tested, costs only 53.8us total) -- roughly 2 orders of magnitude too small to
  matter at any shape tested, not a close call. Decisively ruled out without needing new board time,
  because `PipelineDepth` was already a real, measured csynth quantity, not something requiring a
  fresh probe.
  **EXTENDED, 2026-09-04 (still no board time): the single-region check above only counted `PW_FLAT`
  -- redone summing fill/drain across ALL 4 named pipelined regions a tile/chunk actually walks
  through (`ROW_READ_FILL` depth=3/II=1, `COPY_FROM_ROW` depth=4/II=1, `PW_FLAT` depth=22/II=1,
  `PW_WEIGHT_HOIST` depth=3/II=1 -- all real values from `pwchunk3`'s own csynth reports), weighted
  by each region's REAL call frequency (`ROW_READ_FILL`=`MAC_PR*Cin*n_row_tiles*n_chunks`, the
  highest-frequency region by far; `COPY_FROM_ROW`/`PW_FLAT`=`n_tiles*n_chunks`;
  `PW_WEIGHT_HOIST`=`n_chunks`). **Still only 0.96%-2.26% of the remainder across 4 shapes checked**
  (entry3, entry64, and two of the scaling-experiment's own extreme points) -- even at the highest
  `ROW_READ_FILL` call count tested (12,288 calls), the sum stays 1-2 orders of magnitude below the
  remainder. This is a LOWER BOUND (doesn't capture inter-region FSM transition cost, invisible in
  any per-region csynth report) but small enough that even a substantial unaccounted multiplier
  wouldn't close the gap alone. **Named-region fill/drain, summed across every region a tile visits,
  is decisively insufficient -- something else drives the remainder.**
  **DIRECT MEASUREMENT, 2026-09-04 (ZHR-92, method change after 6 indirect rounds): switched from
  "change X, infer cause from the time delta" to directly observing hardware behavior via RTL
  cycle-accurate cosimulation -- the first time this project's whole PW-remainder investigation has
  measured rather than inferred.** Cosim'd the FULL `mac_array_top` design (both source files, the
  current deployed architecture -- never cosim'd whole before, prior cosim use on this project was
  always an isolated single-function probe) against Group B's smallest shape
  (`pwburst_b1_t16`, cin=48/cout=48/h=8/w=32/n_tiles=16). Needed a new minimal single-call testbench
  (the legacy `mac_array_tb.cpp` can't run against this architecture at all, already documented) and
  a new `COSIM_DEPTH_HINT` ifdef adding explicit `depth=` to every `m_axi` port (cosim, unlike
  synthesis or csim, needs this to know how many elements to transfer between the C testbench array
  and the RTL simulation's memory model -- confirmed via the exact HLS diagnostic, not guessed; depth
  does NOT affect synthesized RTL, a pure simulation hint, confirmed via Xilinx's own documentation).
  **First attempt SIGSEGV'd** -- used byte-count depth uniformly across all m_axi ports, but
  `out_burst`/`in_burst`/`in_base_wide` are `ap_uint<32>`-typed (4 bytes/element) and need depth in
  WORD units, not byte units, unlike the `act_t`/`wt_t` (1-byte-element) ports -- a real, easy-to-miss
  unit mismatch for anyone adding cosim depth hints to a design with mixed-width m_axi ports. Fixed;
  default (non-cosim) build re-verified unaffected both before and after (csim clean each time).
  **Result (`mac_array_top_cosim.rpt`): 94,601 real RTL cycles, PASS, byte-exact.** Compared against
  the analytical compute-only prediction (61,440 cycles) and the real board measurement (219,000
  cycles): **the remainder splits 21.0% RTL-internal (cosim sees it) / 79.0% real-hardware-only
  (cosim's idealized AXI model does not see it, and real hardware costs it anyway).** This is neither
  of the two clean branches the round was designed to distinguish -- not "cosim matches board" (pure
  RTL/scheduling issue) and not "cosim matches analytical" (pure real-hardware issue) -- a genuine
  mixed result, with the majority (79%) confirmed to exist ONLY on real hardware (DRAM controller
  arbitration, the SmartConnect crossbar, or PS-side memory contention -- cosim's own AXI model is
  idealized and doesn't simulate any of these). The smaller 21% RTL-internal share is real too and
  worth its own investigation (a genuine scheduling/FSM cost beyond the flat analytical trip-count
  model), but is not the dominant contributor. **Practical consequence: the next step for the
  dominant (79%) share needs REAL hardware bus instrumentation (a Zynq-7000 AXI Performance Monitor
  instantiated in the BD) to characterize DRAM-side transaction count/latency/throughput directly --
  no further indirect (change-a-descriptor-field, infer-from-delta) round on this codebase's own
  descriptor-construction surface can resolve the real-hardware-only share, since by definition it
  doesn't show up in cosim's own idealized model either.** Not yet attempted (a real BD change, new
  bitstream, and register-read driver code -- a bigger round than any single-op descriptor probe).
  **CHEAPER PRE-CHECK, 2026-09-04, before committing to APM (no BD change needed): computed real
  achieved DRAM bandwidth for the same B1 shape cosim just measured.** Total real DRAM bytes moved =
  activation (`Cin*H*W`=12,288, the whole tensor via `ROW_READ`) + weight (`Cin*Cout`=2,304, one
  `PW_WEIGHT_HOIST` load since `n_chunks=1`) + output (`Cout*h_out*w_out`=12,288) + bias
  (`Cout*4`=192) = 27,072 bytes, moved in 219,000 real cycles (2.19ms). **Achieved bandwidth: 12.36
  MB/s -- 1.55% of the 800MB/s theoretical 64-bit-HP ceiling, 3.09% even against a conservative
  400MB/s (32-bit) estimate. Nowhere close to saturated.** Separately confirmed (from the deployed
  build's own BD tcl, `run_impl_bitstream_pwchunk3.tcl`): `CONFIG.PCW_USE_S_AXI_HP0 {1}` is the ONLY
  HP port enabled -- all 3 masters (`gmem_act`/`gmem_w`/`gmem_b`) funnel through one `smartconnect`
  (`sc_data`, `NUM_SI 3`) into that single HP0 port; HP1-3 are unused. **Because bandwidth is nowhere
  near the ceiling, spreading the 3 masters across the 3 unused HP ports would NOT be expected to
  help on its own** -- that lever only pays off in a bandwidth-saturated regime, which this isn't.
  **This is a latency/arbitration signature, not a throughput-limit signature -- APM is warranted, not
  redundant with what these two cheap checks already show.** Consistent with Group B's own earlier
  finding (remainder grows close to linearly with tile/transaction count, not with byte volume): the
  working picture is many small transactions each paying a real per-transaction DRAM-controller/
  arbitration round-trip cost, not a transfer-size-bound cost. Not yet proceeded to the actual APM
  implementation.
  **DECISIVE PRE-APM CHECK, 2026-09-04, same day: back-solved real per-transaction latency and
  compared to the known 40-80 cycle typical DRAM-read-latency range -- lands at ~34-36 cycles,
  AT/JUST BELOW the typical range, not the "hundreds to thousands" anomalous case.** Total B1
  transaction count computed by type: activation reads (`ROW_READ`'s own `read_request` count,
  `MAC_PR*Cin*n_row_tiles*n_chunks`) = 384; output writes (`out_burst.write_request`, one 4-byte word
  per completed row) = `Cout*MAC_PR*n_tiles*n_chunks` = 3,072 -- **8x the activation-read count for
  this shape**; weight load (burst-inferred) = 3-144 depending on assumed burst length, small enough
  either way not to matter (total transaction count only moves 3,459->3,600, changing the final
  answer by ~4%). Real-hardware-only cycles (from the cosim round, 219,000-94,601=124,399) divided by
  ~3,459-3,600 transactions = **34.6-36.0 cycles/transaction**. This is NOT the anomalous case a
  much-earlier round's own "1,000-1,400 cycles/transaction" figure would suggest -- that number is
  from a different, pre-weight-caching architecture and does not hold under the current one (same
  "measure the CURRENT mechanism" lesson this file already records elsewhere on this exact line).
  **Practical conclusion: normal-ish per-transaction latency, too many transactions -- APM would NOT
  be expected to reveal much new information; the real lever is reducing transaction count.** New
  finding this calculation surfaces: output-write transaction count DOMINATES activation-read
  transaction count on real shapes too (checked analytically, not just B1's own synthetic case):
  entry3 16x, B1 8x, entry64 2x (chunked). This whole multi-round investigation has focused almost
  entirely on activation reads (`ROW_READ`) -- output writeout's own transaction-count burden (one
  4-byte word per completed row, currently) has never been separately identified as a lever before.
  SmartConnect arbitration (3 masters sharing one HP0 port) remains a plausible partial contributor to
  the ~35 cycles/transaction even though bandwidth itself isn't saturated -- a different mechanism
  than the already-ruled-out bandwidth lever, not yet tested in isolation.
  **OUTPUT-WRITE BATCHING OPPORTUNITY, 2026-09-04, same day, computed not implemented: confirmed
  current WRITEOUT granularity is one 4-byte word per write (`out_burst.write_request(byte_addr>>2,
  1)`, triggered once per 4 columns) -- NOT one write per full output row.** `colt*MAC_PC` is baked
  into the write address, so a full row is assembled across `n_col_tiles` SEPARATE `(rt,colt)` calls,
  each issuing its own tiny write -- the 8-16x output/activation transaction ratio found above is
  inflated by exactly this factor, not a natural floor; this line had not hit its end. Real network
  total output-write transactions (all 26 PW layers, `cout*MAC_PR*n_tiles*n_chunks`): **1,001,664**.
  Two batching options computed: (a) tile-batched -- accumulate the full `MAC_PR*MAC_PC`=16-byte block
  before flushing, entirely within one `(rt,colt)` call's own existing scope, no outer-loop
  restructuring -- reduces to 250,416 txns (75.0% reduction), ~260-270ms at 34.6-36.0 cycles/txn; (b)
  row-batched -- accumulate a full `w_out`-byte row across all `n_col_tiles` calls before flushing, a
  real structural change (a buffer whose lifetime spans multiple `pw_flat_pipeline_impl` calls, or a
  loop-nesting restructure) -- reduces to 179,904 txns (82.0% reduction), ~284-296ms. **Both are large
  relative to the 1,806.45ms full-network baseline (14-16%), comparable in scale to this session's
  biggest wins so far.** Open questions before implementing either: alignment for a 16-byte or
  full-row burst (a previously-established 100%-2-byte/50%-4-byte figure doesn't directly answer this
  -- needs its own check), and whether tile-batching's smaller-but-contained win is the better
  near-term target given row-batching's real structural cost. Not implemented this round.
  **CORRECTION, 2026-09-04, same day, before implementing anything: "tile-batched" (the contained
  option above) is not actually achievable -- the memory layout doesn't support it, for any real
  layer.** Row `wr_row`'s 4 output bytes end at `base+wr_row*w_out+3`; row `wr_row+1`'s begin at
  `base+(wr_row+1)*w_out` -- the gap between them is `w_out-4` bytes, zero only when
  `w_out==MAC_PC==4`. Checked all 26 real PW layers' `w_out` values ({1,8,16,32,64}) -- none equal 4,
  so the 4 rows of a spatial tile are NEVER contiguous in DRAM for any real layer; a single burst
  cannot span them. Within one `(rt,colt)` call the code is already maximally batched (one row's own
  `MAC_PC` columns in one word -- the full width that call's own data has); there is no further
  reduction available without either breaking contiguity or spanning `colt` calls. **"Contained
  tile-batching" collapses to "no improvement available" -- it is not a smaller, distinct alternative
  to row-batching; any real reduction needs the identical structural change (cross-`colt`
  accumulation).** The prior entry's ~260-270ms "tile-batched" figure is invalid as something
  achievable without restructuring -- a real miss, caught only when working the follow-up alignment
  question, not before reporting the original number.
  Alignment check (still useful for whatever implementation form follows): row-start addresses
  (`out_off + c*out_ch_stride + oh*w_out`) across all 26 real layers, 131,376 addresses checked --
  **mod4==0 (word-aligned): 100%** (excluding narrow layers 50/51, `w_out=1`, which never use
  `out_burst`/`FAST_WRITEOUT` anyway); mod16==0: 89.48% overall, with the only misaligned rows
  belonging to `w_out=8` layers (40/43/44/47/48) in a clean, predictable 50/50 even/odd-row split
  (row stride=8, not a multiple of 16) -- not scattered. Given full-row batching is the only real
  option, a FIXED 4-word (16-byte) burst isn't even the right shape to aim for -- real row lengths
  (`n_col_tiles` in {16,8,4,2,1}) don't divide evenly into "groups of 4 column-tiles" (w=8 layers'
  whole row is only 2 words; w=16's whole row already fits one 4-word group). **The clean, uniform
  implementation is a variable-length word burst per row** (`write_request(addr, ceil(w_out/4))`,
  runtime length), matching this project's own already-validated read-side precedent
  (`row_hoist_probe`'s `hls::burst_maxi::read_request` with genuine runtime length, confirmed
  `ManualBurstInstancePassed, Length=variable`) -- not a fixed-length 4-word burst. Alignment is clean
  for this (100% word-aligned) -- no dual-path/mixed-alignment case is triggered by this check.
  **Net position: alignment isn't the blocker, but the achievable option is confirmed to be
  row-batched only (82% reduction, ~284-296ms) -- the structural-change risk this round's own
  decision was trying to avoid by picking "tile-batched" cannot actually be avoided while still
  getting a real reduction.** Not resolved as of this entry -- awaiting a decision on whether to
  proceed with the structural change (this line's own poor track record with restructuring: PW
  tiling's own resource blowup, `PW_WCHUNK`'s 3-round P&R chase, 3 failed DATAFLOW attempts, all
  elsewhere in this file) or close this opportunity out.
  **General lesson**: before reporting a "contained, no-restructuring" option's own savings figure,
  verify the underlying DRAM ADDRESSES the batched write would actually need to be contiguous are
  genuinely contiguous for the real shapes in question -- a batching idea that looks structurally
  cheap (same loop nesting, same call scope) can still be physically invalid if the data it would
  need to combine isn't adjacent in memory, and this only shows up by checking the real stride
  arithmetic, not by reasoning about which loop the code lives in.
  **STANDING RULE, confirmed 2026-09-04: for ANY "batch multiple accesses into one wider transaction"
  proposal on this codebase, contiguity must be checked FIRST, before computing benefit -- benefit
  math on data that turns out non-contiguous is wasted work, caught here only by chance while
  answering a DIFFERENT (alignment) question, not because contiguity was checked deliberately up
  front.** This is not a one-off miss -- it is the same underlying discipline as this file's own
  "runtime value gating a hardware region" and "check history before measuring" rules, applied to a
  new surface (memory layout, not control flow or prior measurements). Order of operations for any
  future batching proposal: (1) derive the real byte-stride/address formula for the specific shapes
  in question, (2) confirm the target span is actually contiguous (stride between consecutive
  elements equals the element size, not a larger gap), (3) only then compute alignment and benefit.
  Skipping step 2 and going straight to benefit math is what nearly shipped an invalid ~260-270ms
  estimate here.
  **FOLLOW-UP, 2026-09-04, next round: the AXI-transaction-count hypothesis was tested and REFUTED in
  its simple linear form -- but the data shows a real, non-proportional effect instead, not a clean
  null result.** 3 synthetic bundles, holding `Cin` FIXED (48, not varied against W_in as the round's
  own first design sketch suggested) and trading `H_in` against `W_in` -- strictly better than varying
  Cin, since it eliminates the `n_cbase` confound entirely (Cin fixed -> n_cbase=2 identical across all
  3) rather than needing to model and subtract it; `n_tiles`(=32) and total bytes(24,576) fall out
  AUTOMATICALLY constant once Cin and total bytes are both fixed, and analytical PW_FLAT compute time
  is therefore IDENTICAL by construction (1.2288ms) across all 3 -- no subtraction uncertainty. Total
  ROW_READ transaction count varied 1x/2x/4x (384/768/1536) via burst size (n_words 16/8/4). csim
  14/14, board 6/6 (3 points x2 reps) byte-exact, repeatability <=0.01ms.
  **Result: 1x->2x (transactions doubled) showed essentially ZERO change in the remainder (+0.2%);
  2x->4x (transactions doubled again) showed a real +52.5% remainder jump.** Over the full 4x
  transaction range, remainder only grew 1.53x, far short of the 4x simple proportionality predicts --
  and the pattern (flat, then a jump) isn't even monotonic with the ratio the way "more transactions
  costs proportionally more" would need. **Simple linear scaling with transaction count is refuted**,
  but this isn't a clean null result either -- there IS a real, repeatable, non-noise effect at the 4x
  point. The one variable that changes monotonically and lines up with where the jump happens is BURST
  SIZE (n_words per transaction: 16/8/4) rather than transaction COUNT itself -- the 4x point's 4-word
  bursts may cross an AXI/burst-efficiency threshold the 8- and 16-word points don't. This is a
  post-hoc read of the pattern, not yet tested as its own isolated hypothesis (would need 2 shapes with
  the SAME transaction count but different burst sizes, mirroring how this round isolated transaction
  count from byte volume) -- flagged, not confirmed. **Lesson for this project's own scaling-experiment
  method**: designing a test to isolate variable X (here, transaction count) via a compensating
  variable Y (burst size) can accidentally reveal that Y itself was the real driver, not X -- a clean
  experiment on the INTENDED variable is exactly what surfaces this, whereas a confounded design
  wouldn't have separated the two possibilities at all.
  **FOLLOW-UP, 2026-09-04, next round: attempting to isolate "burst size" (the new hypothesis from
  the entry directly above) hit a STRUCTURAL IDENTITY, not just another confound -- `n_words`
  (ROW_READ's own burst-length formula, `ceil(w_in/4)` under byte-aligned addresses) and
  `n_col_tiles` (`ceil(w_out/MAC_PC)`, `MAC_PC=4`, `w_out=w_in` since PW is always k=1/s=1/p=0) are
  THE EXACT SAME FORMULA, always, whenever `MAC_PC=4` matches the `/4` in the burst-length math --
  not a coincidence of any one test design, a property of the current source code itself.** Burst
  size and column-tile count are not two separable variables in this codebase; they are one. A
  design holding `n_tiles`(`=n_row*n_col`) fixed via `h_in`-vs-`w_in` compensation (attempted this
  round, and -- realized only in retrospect -- also the PRIOR round's own "burst size" design)
  necessarily varies `n_col_tiles` (hence `n_words`) AND `n_row_tiles` (hence `ROW_READ`'s own call
  count) simultaneously, since holding their PRODUCT fixed makes them trade off -- it cannot isolate
  either one. **Both this round's and the PRIOR round's own "burst size" readings should be treated
  as reflecting the COMBINED `n_words`/`n_col_tiles`/`n_row_tiles` variable, not burst size
  specifically -- neither round actually separated it.** A genuinely clean sub-experiment fell out of
  this same round almost by accident: holding `w_in` (hence `n_words`/`n_col_tiles`) FIXED and
  varying only `h_in` (hence `n_row_tiles`) shows the remainder grows close to linearly with
  `n_row_tiles`/`n_tiles` (a 2-point linear fit, `remainder ~= 0.684 + 0.0557*n_tiles`, predicts a
  3rd held-out point within ~5%) -- a real, substantial, roughly-linear-in-call-count effect,
  independent of burst size, and NOT itself a threshold effect. Whether burst size matters
  independently of column-tile count remains genuinely untested -- no descriptor-only construction
  can separate them, since they are the same number by construction; separating them for real would
  need a SOURCE-LEVEL change (e.g. a probe that forces a different read granularity than `MAC_PC`),
  not a new synthetic shape. **General lesson, adds to the entry above rather than replacing it: before
  designing an experiment to vary "X while holding Y fixed via a third quantity Z," check whether X and
  Y are actually defined by the SAME formula under the conditions being tested -- two quantities that
  look conceptually distinct (a burst length in words; a spatial tile count) can turn out to be
  mathematically identical once the specific constants involved (here, both dividing by 4) are
  substituted in.**
  **IMPLEMENTED AND REVERTED, 2026-09-04, next round: row-batched WRITEOUT closed real P&R timing and
  stayed byte-exact correct, but made real board time WORSE by +142%, not better -- a genuine,
  reproducible dead end, not a resource or correctness failure.** Per the decision made two rounds
  above (tile-batching ruled out by the contiguity finding; row-batching chosen over the smaller
  tile-batched option specifically because it reuses the existing `colt` loop instead of adding a new
  one, avoiding `PW_WCHUNK`'s own 3-round shared-multiplier P&R chase): `colt` stays the outer loop,
  unchanged; `pw_flat_pipeline_impl`'s FAST_WRITEOUT path now stores each computed byte into a
  persistent on-chip buffer (`pw_out_row_buf[MAC_PR][MAX_COUT_TIMES_WOUT]`, new constant
  `MAX_COUT_TIMES_WOUT=9216`, a real product-bound across all 26 layers -- same reasoning as
  `MAX_CIN_TIMES_W`, a coincidentally identical value from independent boundary layers) instead of
  bursting immediately; a new `PW_WRITEOUT_FLUSH` loop in `run_layer`, once per `(chunk,rt)` after the
  entire `colt` sweep completes, issues ONE variable-length burst write per `(ot,row)`
  (`write_request(addr, ceil(w_out/4))`, matching the read-side `ManualBurstInstancePassed` precedent).
  `ot_row_base`/`pw_ot_row_base` follow the same "accumulated via `+= d.w_out`, never multiplied inside
  a pipelined function" discipline as `ot_out_ch_base` -- deliberately avoiding `PW_WCHUNK`'s own
  shared-multiplier-FSM regression mechanism by construction, and it worked: **real P&R closed clean on
  the FIRST attempt, no chase needed** (WNS=+0.091674ns, vs baseline's +0.153ns -- thinner margin but
  positive; LUT 32,948/53,200=61.93%, +995/+1.87pp; BRAM 90/140=64.29%, +16 tiles, matching the isolated
  csynth BRAM estimate almost exactly this time -- unlike most of this project's isolated-vs-real BRAM
  history; DSP 51/220=23.18%, roughly flat). csim 6/6 + 20/20 (pw_weight_hoist_tb.cpp +
  pw_scaling_probe_tb.cpp) both clean on first try after fixing one self-caught bug (the flush loop's
  own row-base accumulator was initialized to a literal `0` instead of the chunk's real absolute base
  `pw_ot_row_base` -- would have broken chunked layers 43/44/47/48 past their first chunk; caught by
  code review before running csim, not by a failed test).
  **Real board (2026-09-04): entry3 (cin=cout=48, 64x64, the same degenerate/non-fallback shape the
  PW_WCHUNK round called "unchanged at 22.67ms") came back at 55.01-55.09ms across 2 repeats -- a
  reproducible +142% REGRESSION, not the expected reduction.** entry64/entry60 both stayed byte-exact
  (0 mismatches) but were not timed further once entry3's regression was confirmed reproducible --
  stopped and reverted immediately per this project's own one-round-at-a-time discipline, rather than
  running the full network to get a more precise magnitude on a result whose DIRECTION was already
  clear and decisive. Board reverted to the `pw_wchunk` baseline bitstream (re-confirmed 22.68ms,
  byte-exact); golden rollback image md5 unaffected (`7ee26f67a1fca38a2752e99cf0bac25b`, unchanged).
  **Root-cause hypothesis (design-level, not yet independently confirmed via cosim or a binding-report
  read): the OLD per-4-byte inline write was issued from inside the already-running II=1 compute
  pipeline, so its AXI cost was fully hidden behind compute latency -- this is exactly what the much
  earlier `PW_FIX_OUTADDR` fixed-address probe already found (~0% cost, output-write address contributes
  nothing measurable). `PW_WRITEOUT_FLUSH` is a NEW, separate, sequential stage that runs only AFTER the
  whole `colt` sweep for a given `rt` finishes, with nothing else scheduled to overlap it. It cut burst
  COUNT exactly as designed (3,072 row-bursts vs 49,152 word-bursts for entry3's shape, a real 16x
  reduction, matching the pre-registered transaction-count math from two rounds above almost exactly),
  but every one of those fewer bursts now pays its own real, previously-hidden `write_request`/
  `write`/`write_response` handshake latency as a serial, non-overlapped cost -- and that per-transaction
  overhead evidently outweighs the 16x count reduction by a wide margin.** This is the SAME mechanism
  this file's own "csynth region-level reports omit sequential glue cost" rule already describes
  (elsewhere in this file, the `run_layer` DW_PATCH_STAGE glue-cost entry) applied to a NEW surface:
  not just that isolated per-region csynth numbers miss glue cost, but that a mechanism which is "free"
  ONLY because it's interleaved into an existing pipeline can become expensive again the moment it's
  pulled OUT into its own sequential stage, even while objectively reducing the raw operation count.
  **Standing lesson for any future batching/hoisting proposal on this codebase: reducing transaction
  COUNT is not sufficient by itself if the batched operation moves from an already-pipelined,
  latency-hidden context into a new serial stage -- the per-transaction latency that was free inside the
  pipeline is not free once it's outside it.** Before proposing another batching/hoisting move, check
  whether the current mechanism's cheapness comes from pipeline overlap specifically (not just "it's
  inside a `PIPELINE` region") -- if so, any restructuring that pulls the operation out of that overlap
  needs its own overlap story (e.g. double-buffering so the NEXT iteration's compute can run concurrently
  with the flush), not just a smaller operation count. **This closes the output-write-batching line for
  now** -- the 82% transaction-count reduction was real and achieved, but the wrong lever: this
  investigation's real finding is that output-write cost is NOT primarily transaction-count-bound the
  way weight-read was: it's bound by whether the write stays inside the pipeline's existing overlap, a
  structural property the count-reduction math never modeled. `mac_array_a3_pw_wchunk` (1,806.45ms)
  remains the deployed baseline; not superseded by this round.
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
  **Confirmed a SECOND time, same day (ZHR-92, fallback-4 PW_FIX_WADDR probe, entries 64/66/70/72):**
  same technique, different build (WNS=-0.969ns route-only vs. -0.464ns after `phys_opt_design`,
  again a measurably different physical implementation) -- all 4 entries board-tested on both builds
  came back with matching timing (24.82/25.90/21.59/22.67ms both times) and identical mismatch counts
  (18450/19135/11983/2106, every digit). This is now a repeatable, general-purpose technique on this
  project, not a one-off -- reach for it by default whenever a timing-only probe (values allowed to be
  wrong, timing is the real question) comes back WNS-negative, rather than treating a negative-WNS
  probe result as automatically unusable.
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
  **REFINED, 2026-09-03 (ZHR-92, chunked PW weight-loading round): a new OUTER loop level (not just a
  new call site at the same nesting depth) can restructure a function's own FSM enough to change how
  IT ITSELF competes for the shared multiplier -- the same symptom (WNS regression, same sink resource)
  can have two DIFFERENT specific causes at two different points in one restructuring, needing two
  separate fixes, not one bigger one.** Adding `PW_WCHUNK` (wrapping the entire `(rt,colt)` spatial
  sweep in `run_layer`, needed for chunked/multi-load PW weight caching) triggered this twice in
  immediate succession. Round 1: `ot_out_ch_base = ot_start * d.out_ch_stride`, computed inside
  `pw_flat_pipeline_impl`'s own init (called once per spatial tile, up to 4x per chunk), real P&R
  WNS=-2.264415ns -- critical path source was `pw_flat_pipeline_impl_false`'s OWN separately-
  synthesized FSM feeding `mul_32s_32s_32_2_1`, ~10x worse than every prior instance of this mechanism
  (historically -0.1 to -0.3ns) because the new call site sat inside a sub-function's own FSM
  (physically farther from the shared multiplier) rather than `run_layer`'s own top-level FSM. Fixed:
  moved to a genuine loop-carried accumulator in `run_layer`'s own `PW_WCHUNK` loop, stepped once per
  chunk (not per tile) by each chunk's REAL clamped count (robust to an uneven last chunk by
  construction, not by relying on "only the last chunk is ever partial" holding). Real P&R:
  WNS=-1.127835ns -- improved (roughly halved) but STILL NEGATIVE. Round 2, same day: the critical
  path had MOVED -- source was now `run_layer`'s OWN top-level FSM state, sink still the SAME
  `mul_32s_32s_32_2_1`, but via a DIFFERENT pre-existing operation (`pw_flat_pipeline_impl`'s own
  `total_iters` computation, loop-invariant across the whole `(rt,colt)` sweep for a given chunk, but
  previously recomputed once per tile). This was NOT the two new accumulator multiplies from round 1's
  fix (those live in a separate, low-frequency scope) -- `PW_WCHUNK`'s own new loop-nesting level had
  itself changed `run_layer`'s FSM structure enough to alter its competition for the shared multiplier,
  independent of round 1's specific fix. Fixed the same way (hoisted `total_iters` to `PW_WCHUNK`,
  passed in as a parameter) -- bind-database check confirmed ZERO `Multiplier`-core operations remain
  in either `pw_flat_pipeline_impl` instance afterward. Real P&R: **WNS=+0.133715ns -- closed.**
  **Practical rule: when a WNS regression's fix only partially recovers the margin, re-pull the actual
  critical-path report before assuming the same fix needs to go further or a different lever (pblock/
  phys_opt) is needed -- the source/sink netlist names will say whether it's the same mechanism
  recurring (same source) or a genuinely different one exposed by the same structural change (different
  source, same sink). Both rounds here were diagnosed this way, not guessed, and each fix was surgical
  because of it.**
- **Getting a multiply OFF the shared 32x32 unit -- the technique list, with the one that does NOT
  work (2026-09-12, `SHARED_MUL_ARMS` round).** Two techniques have each been verified in the
  binding DB (`*.verbose.bind.rpt` opset) AND the exported module list, which is the only way to
  judge them -- instance count and csim are both blind to this:
    1. **Loop-carried accumulator, INCREMENTING form only**: the product is `index*stride` where
       `index` steps by 1 inside a real loop -- replace with `base += stride` stepped in that loop
       (rt/rr row bases, `wr_row_off` next to `wr_row`, `pw_ot_lo += per_chunk`). Used successfully
       6+ times on this line (PW_WCHUNK's two fixes, ot_out_ch_base, the three this round).
    2. **Narrow operand types**: `ap_uint<11> x ap_uint<15>` etc., sized from the real descriptor
       ranges with csim asserts on the ranges -- HLS emits a DIFFERENT core (`mul_11ns_15ns_26_1_1`,
       `mul_20ns_9ns_21_1_1`) that structurally cannot be bound onto `mul_32s_32s_32`, so the op
       leaves the shared unit's mux without any `ALLOCATION` pragma (silently ignored once here).
       Cost: the small `_1_1` cores are LUT-fabric multipliers (~+100-200 LUT each in real P&R),
       DSP goes DOWN. Verified twice in one round (top-level `scalar_hw`/`total`, run_layer's four
       per-chunk products).
    X. **A standalone add-loop ("add the invariant N times") does NOT work**: HLS's loop-idiom
       pass rewrites `for (i<N) acc += c` into `N*c` -- an i32 multiply, bound straight back onto
       the shared unit (binding DB showed `mul_ln1056/_1/_2` with the loop gone). The accumulator
       technique only survives when the accumulated quantity is stepped alongside REAL work in the
       loop, not summed in a loop that exists only to sum. Caught in-round by re-checking the bind
       DB after the first csynth, not by csim (which passes either way).
- **Isolated-csynth EXTRAPOLATION is unreliable (10+ instances above) -- but the SAME MECHANISM's
  own historical isolated/real ratio can be usable, given two or more data points.** Confirmed
  2026-09-13: before re-running P&R on DW_OUTPUT_BURST-on-SHARED_MUL_ARMS, the "will real LUT go
  over 85%" question was answered not from the isolated number (which had just been wrong in SIGN
  on the previous round, -283 isolated vs +568 real) but from this exact mechanism's two prior
  real P&Rs: isolated -1,162 -> real -280 and isolated -1,290 -> real -334 (both negative, both
  ~1/4 of isolated). Prediction: ~44,300. Real: 44,080. First time on this line a resource number
  was forecast and hit. The distinction from the general rule: the general "isolated is
  direction-agnostic-unreliable" verdict is about extrapolating ACROSS mechanisms (a new change's
  isolated delta says little about its real delta); WITHIN one mechanism, re-built on a different
  base, the isolated/real relationship has now held on three consecutive builds. Use it only with
  >=2 prior real data points for the same mechanism, only for the same resource category, and say
  which prior builds the ratio came from.
- **The "gate it OFF, keep it" convention has a payoff beyond documentation: a rejected round's
  INFRASTRUCTURE (a port, its driver register, a testbench hook) can be reused directly by a later
  round -- so do not strip it when gating the mechanism.** Confirmed 2026-09-14 (`DWR_ROWREAD`):
  the row-granularity input-read fix needed a 32-bit `burst_maxi` read port on `gmem_act` for DW.
  `dw_in_burst` -- added by the REJECTED `DWR_INPUT_BURST` round (2026-09-07), kept as a live port
  with its AXI-Lite register (0x100) already wired into all three ARM call sites, and `(void)`-ed
  in the default build ever since -- was exactly that port: zero interface change, no new adapter,
  no new register, no driver rebuild, no chance of the "shared bundle != shared register" trap.
  Reusing `in_burst` (the first idea) would have needed a signature change through
  `run_dw_layer_raster`. When a round is gated OFF, list in its comment what infrastructure it
  leaves behind (ports, registers, tb hooks) so a later round can find it.
- **THE TWO-STEP PROCESS that decided three DW rounds in two days (2026-09-13/14: DW 329 -> 225 ->
  115.5ms, entry5_dw 38.9 -> 5.4ms) -- reusable, and each step is cheap. Use it for any
  "an AXI access inside a pipeline is expensive" question.**
    **Step 0 -- decompose from BOARD data, never from csynth or comments.** Take the last full-
    network run's per-entry ms for every real layer of the operator, and fit them to 2-3
    PHYSICAL counts from the descriptors (input pixels, outputs, rows/channels/tiles; the loop's
    own trip count as the floor). `tools/decompose_full_network_log.py` gives the per-entry ms;
    a 10-line numpy `lstsq` gives the terms. Read the coefficients as cycles-per-thing and
    compare to the II floor; the term far above its floor is the target. R^2 should be >0.99
    with physical terms -- if it is not, the model is missing a term, not the data.
    **Step 1 -- schedule-shape probe BEFORE implementing (csynth only, ~5 min).** Write the
    minimal variant and count, per module, where `readreq`/`read`/`writereq`/`writeresp` land in
    `*.verbose.sched.rpt` (`grep -c` per file). The shape that works: request and response in
    small loops OUTSIDE the pipelined loop, the data op (`read()`/`write()`) still INSIDE it at the
    same II. The shape that fails (PW_WRITEOUT_FLUSH 2026-09-04, +142%; DWR_INPUT_BURST
    2026-09-07, +43ms): the DATA op leaves the pipeline into a serial stage. If the data op
    moved, stop -- no csim, no P&R. This catches the failure in 5 minutes that those two rounds
    caught after a full P&R + board each.
    **Step 2 -- pre-register the board landing point as a RANGE with a reading per sub-range,
    then read the II-materialisation per layer.** For each layer, did the cycles-per-thing drop by
    the amount the fix predicts? A 1:1 drop = that side was binding and is now fixed; a
    shortfall concentrated in the fast layers = the OTHER side of the producer/consumer pair is
    now binding (this is what found produce after ROWBURST and drove ROWREAD); a uniform
    shortfall = a third cost source, refit. Then re-fit (step 0 again) on the new run so the next
    round starts from measured terms, not the previous round's projection.
  Every round of this line since the first ROWBURST probe followed exactly this, and the
  pre-registered ranges bracketed every real landing point.
- **"Use the top-N timing list to predict the next bottleneck" is NOT reliable on this design --
  placement variance is larger than the spacing between the near-tied paths.** Confirmed
  2026-09-12: the 300-path report on `sohoist` put the next-worst distinct structure at +0.246
  (DW -> `gmem_act` store FIFO) and the prediction "remove the sink -> WNS ~ +0.25" was made from
  it. After the sink was actually removed (`SHARED_MUL_ARMS`), WNS was +0.128, the worst path was
  one (`dwr_produce2`'s `ROW_COL` pointer carry chain) that had NOT appeared anywhere in the
  previous top-300, and the +0.246 path was not in the new top-8. The ordering of the
  route-dominated population (+0.13..+0.45 at 10ns) is re-rolled by every placement, so a rank
  read off one build's report is a sample, not a forecast. Use such a list to say "there is a
  population of N structures within X ns" (that held), never "the next one is Y at Z ns."
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
  **New symptom confirmed 2026-09-04 (ZHR-92, PW burst-size round): a hang can present as SSH itself
  going unresponsive (timing out during banner exchange) while ICMP ping keeps answering normally
  (0% loss, ~2-3ms RTT) — network layer alive, board's own SSH/system layer not.** Same recovery
  path applies. **Adapted step 2 this round**: no local raw `.bit` for the literal golden image was
  available in-session (it predates this session, only the swapped `.bin` and its md5 were on hand,
  and `Overlay()` requires a raw `.bit` as input — it converts and writes the `.bin`, it does not
  accept an already-swapped `.bin` back in, confirmed by the `AssertionError: expected 'a' tag, got
  b'\x00'` when tried). Substituted the current deployed baseline's own `.bit` (present locally from
  its own earlier real P&R this session) as the PL-reconfiguration proof-of-life instead — a
  reasonable adaptation when the literal golden `.bit` isn't available, since it's an independently
  real-P&R-verified, already board-tested build; the golden `.bin` FILE itself was still re-verified
  by md5 (unchanged) even though it wasn't the one reloaded.
  **Observed pattern, root cause NOT known -- recorded so it's not re-derived from scratch next time:
  this is the 2nd occurrence of the SSH-dead/ping-alive hang symptom, and both times it happened
  during a round doing MANY consecutive board dispatches in one session** (the first was a full-
  network 82-entry run; this one was after a long sequence of single-op scaling-probe dispatches).
  Not yet confirmed as causal (could be coincidence -- both are also simply the rounds with the most
  total board time), but worth treating "many consecutive dispatches in one sitting" as a mild risk
  factor until either a real mechanism is found or enough hang-free long sessions accumulate to make
  the correlation look like noise.
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
  **150MHz RE-CHECKED, 2026-09-05 (ZHR-92, MAC_PD=4 architecture, 3-point sweep with HLS and Vivado
  BOTH tightened together): the whole investigation line above (this "Sixth confirmed instance" entry
  and its own "two nearly-tied critical paths, HLS swings between them, neither below ~8.9ns" verdict)
  was measured on the PRE-gmem_meta-elimination, MAC_PD=1 architecture -- both of that verdict's
  premises are gone (gmem_meta doesn't exist; the critical path moved to `mul_32s_32s_32_2_1`, then to
  the whole different mechanisms below at MAC_PD=4). Re-ran the same style of sweep the ORIGINAL
  111/125/150MHz round used, but fixing that round's own gap (it only tightened the Vivado-side
  constraint on RTL scheduled for a fixed 10ns HLS target) -- this time `create_clock` (HLS) AND the
  PS7 FCLK/XDC (Vivado) were retargeted together at each of 3 points: 9.0ns/111MHz, 8.0ns/125MHz,
  6.67ns/150MHz, real P&R (route_design only) at every point, on the CURRENT deployed baseline's
  source (`mac_array_a3_macpd4`, MAC_PD=4, WNS+0.339ns at 10ns/100MHz).
  **Absolute critical-path data-path delay across the 3 points: ~8.7-8.8ns -> ~8.26-8.40ns ->
  ~8.1-8.3ns.** Real, substantial rescheduling improvement between the first two points (~0.4-0.5ns
  drop, confirming HLS-side rescheduling is a genuine lever on THIS architecture too, not just the
  8ns-vs-10ns finding this entry already established on the old one) followed by an near-total
  PLATEAU between the last two (~0.1-0.2ns, essentially floored) -- the same two-phase shape
  (real-then-floored) as the original 8ns/6.67ns finding, but at a LOWER floor: ~8.1-8.3ns
  (~120-123MHz) here vs. the old architecture's own ~8.9ns (~112MHz) floor -- real, structural
  progress, but still well short of 150MHz's 6.67ns requirement.
  **The specific DOMINANT critical-path mechanism changed at every single point -- three different
  mechanisms across three points, not one recurring bottleneck:**
    - 9.0ns/111MHz (WNS=+0.008764ns, barely closed): 9 of the top 10 paths are `pw_flat_pipeline_
      impl_false`'s own MAC multiply-accumulate carry chain (`mul_8s_8s_16_1_1` feeding `acc_*_reg`,
      11-13 logic levels, mostly `CARRY4`, 46-48% logic/52-55% route) -- the actual per-lane compute
      hardware itself, not address arithmetic, not an AXI FIFO. This is the FIRST time in this
      project's whole timing-investigation history that the dominant critical path is inside the real
      MAC datapath rather than staging/glue/address logic.
    - 8.0ns/125MHz (WNS=-0.650887ns): same mechanism, but the dominant INSTANCE switched from
      `_false` to `_true` (8 of 10 paths), with a 2-path outlier (a `LayerDescV2` descriptor BRAM read
      feeding `_false`'s own `empty_reg`, only 3 logic levels but 57% route -- a placement-distance
      signature).
    - 6.67ns/150MHz (WNS=-2.280775ns): switched again, this time to a COMPLETELY DIFFERENT subsystem
      -- `gmem_act_m_axi`'s own AXI write-response FIFO (`store_unit_0/user_resp/dout_vld_reg`
      fanning out into multiple SRL-based FIFO bit-slices), 9 of 10 paths, 22% logic/77-86% route (a
      pure placement-distance signature, not logic depth).
  **Practical reading: each point is individually CONCENTRATED (one dominant mechanism owns 80-90% of
  the top 10, matching the original sweep's own "concentrated, not spread" finding), but the
  mechanism that's concentrated keeps changing as the constraint tightens -- multiple near-tied
  critical paths trading places, the same qualitative pattern the pre-gmem_meta-elimination sweep
  found (there: gmem_meta's own FIFO vs. `dwr_consume6`'s FIFO), just with entirely different specific
  paths this time (the old paths don't exist in this architecture at all).** This recurrence across
  two architecturally very different builds suggests "several genuinely different, near-tied critical
  structures that swap places as HLS reschedules for a tighter target" may be a general property of
  how this whole design's timing closure behaves under pressure, not a coincidence tied to gmem_meta
  specifically. **CONFIRMED A SECOND TIME, 2026-09-12, at a FIXED 10ns constraint (not a frequency
  sweep): the DW_OUTPUT_BURST 6th-port and port-reuse builds (route-only -0.418/-0.461ns) vs the
  deployed baseline (-0.167ns) and the DWR_INPUT_BURST build (-0.103ns) all have byte-identical
  `gmem_act` adapter RTL and structurally identical shared-multiplier operand muxes -- the whole WNS
  spread is route delay (3.42 -> 3.65 -> 3.83ns, logic flat at 5.17-5.29ns) on two pre-existing
  near-tied arms into the same sink (`run_layer`-FSM-sourced, 4 levels, vs `desc_op_type`-sourced,
  6 levels) trading places under placement. So the phenomenon is not specific to tightening the
  clock -- ANY netlist change reshuffles which near-tied arm is worst, with ~+-0.3ns of variance,
  as long as the sink itself has zero margin. The fix that actually worked (same day, below) was
  to shrink the sink's own operand mux, not to touch either arm.** Resources grew monotonically with
  tighter constraints as expected (LUT 81.03% ->
  81.55% -> 83.67%, BRAM/DSP unchanged at every point) -- consistent with "tighter timing needs more
  pipeline stages," never itself the blocking factor at any of the 3 points (no DRC-level resource
  failure). **Verdict: 150MHz is NOT achievable on this RTL structure via HLS/Vivado clock-target
  tightening alone -- the floor is real (~8.1-8.3ns / ~120-123MHz), improved from the old
  architecture's own floor, but a different lever (not more rescheduling) would be needed to close
  the remaining ~1.4-1.6ns gap to 6.67ns.** 111MHz is achievable (barely, WNS=+0.009ns, no margin for
  error); 125MHz is close but not yet closed (WNS=-0.651ns) and would need a real fix (not just a
  retry) on the MAC-accumulator-carry-chain mechanism specifically.
  **MARGIN SEARCH, 2026-09-05, same round, follow-up: found a real-margin candidate frequency via P&R
  (106.667MHz, WNS=+0.212ns, clears a >=+0.15ns bar), but real board deployment revealed a FUNDAMENTAL
  DEPLOYMENT-METHODOLOGY BLOCKER that has nothing to do with timing closure -- changing a bitstream's
  own embedded PS7 IP `PCW_FPGA0_PERIPHERAL_FREQMHZ` config does NOT change the real PL clock
  frequency on this board's actual running hardware.** First checked achievable real frequencies (the
  Zynq-7000 IO PLL's divider granularity doesn't hit arbitrary requested values): querying the PS7
  IP's own `CONFIG.PCW_ACT_FPGA0_PERIPHERAL_FREQMHZ` readback property confirmed real achievable
  points near the target range are 100.000000, 106.666664, 107.692307, 109.090912, 111.111115 MHz --
  NOT a continuum, and NOT matching arbitrary round numbers like "105" or "108" (both requests snap to
  a nearby achievable divisor ratio). Built and board-tested a full bitstream at the best candidate
  (106.667MHz, WNS=+0.212ns, real P&R -- though even at this point the top-10 critical paths were
  scattered across 3 DIFFERENT mechanisms within ~0.15-0.35ns of each other -- `gmem_w`'s AXI load
  buffer feeding directly into DW's gather stage at 0 logic levels/94% route, `pw_flat_pipeline_impl`'s
  own accumulator carry chain, and a `LayerDescV2` descriptor-RAM write-enable path -- a multi-way tie,
  not one clean bottleneck, flagged as elevated risk before deployment; deployed anyway since every
  path was independently diagnosed, not an unknown unknown).
  **Real board result: full-network time was UNCHANGED (1,524.43ms vs. the 100MHz baseline's
  1,522.39ms, a +0.13% difference) instead of the expected ~6.25% improvement (~1,427ms projected from
  the clock ratio).** This is not measurement noise -- a 6.25% (~95ms) difference is far outside every
  noise band this project has ever documented for this kind of measurement (single-op repeatability
  0.01-0.06ms; even scaled to a full 82-entry run, nothing close to 95ms). **Root cause: on Zynq-7000,
  PS7 clock generation (the FCLK0-3 dividers) is configured by the boot flow (FSBL/u-boot's PS7 init)
  ONCE at cold boot, and is architecturally independent of whatever a LATER bitstream's own embedded
  PS7 IP customization requests -- reconfiguring the PL fabric at runtime via Linux's
  `/sys/class/fpga_manager/fpga0/firmware` interface (this project's entire deployment method,
  established since the very first bitstream swap) touches ONLY the PL fabric, never the PS clock
  tree.** Checked for a live runtime clock-reprogramming path (the classic PYNQ-style `fclk` sysfs
  interface, or the standard Linux `clk` framework's debugfs `clk_summary`) -- neither exists on this
  board's kernel (`6.6.40-xilinx`, `debugfs` not even mounted, no `fclk*` sysfs nodes found anywhere
  under `/sys/devices` or `/sys/class`). A direct SLCR register read (`devmem 0xF8000170`, the
  `FPGA0_CLK_CTRL` register) was attempted as an independent hardware-level cross-check but the
  decoded divisor values didn't cleanly resolve to a specific frequency with confidence (possible
  bit-field mis-decode on this attempt) -- NOT relied upon; the empirical board-timing evidence above
  is the load-bearing evidence for this finding, not the register read. **This is the FIRST time in
  this project's whole history that a non-100MHz bitstream was ever pushed through `write_bitstream`
  and deployed to real hardware** -- every prior 111/125/150MHz round (both the original gmem_meta-era
  sweep and this same round's own earlier 3-point sweep) was explicitly P&R-only ("timing-recon, no
  bitstream," per this project's own established convention for that class of round), so this
  discovery does NOT retroactively invalidate any prior claim -- checked directly (grepped CLAUDE.md
  for any prior "board" + frequency-value co-occurrence): none exists. **Practical consequence: EVERY
  WNS/timing number this whole 150MHz investigation line has ever produced (the original sweep, the
  HLS-reschedule-to-8ns round, this round's own 3-point sweep and margin search) is a real, valid P&R
  static-timing-analysis result -- but NONE of them describe a frequency that has ever actually been
  proven to run on real silicon, because achieving that requires a boot-level change (modifying
  BOOT.BIN/FSBL's own PS7 init parameters) and a physical reboot, not just a bitstream swap. This is a
  categorically different, higher-risk class of action than anything done on this board so far this
  entire session (every previous deployment has been a safe, reversible PL-only bitstream swap) --
  not attempted this round, and should not be attempted without explicit user authorization given the
  risk of an incorrectly-modified boot image leaving the board unable to boot at all, needing physical
  recovery.** Board reverted immediately to the known-good `mac_array_a3_macpd4`/100MHz deployed
  baseline (re-confirmed 17.30ms on entry3, byte-exact) once this was established; the 106.667MHz
  bitstream is not promoted and is not the deployed baseline. **This closes out the "find a headroom
  frequency" sub-line for this session — not because no such frequency exists (106.667MHz's P&R result
  is real and would very likely deliver its projected ~6.25% gain if a real boot-time clock change were
  made), but because actually realizing it needs a fundamentally different, higher-risk class of change
  that this session's own safety discipline doesn't authorize on its own initiative.**
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
- **LUT occupancy percentage across genuinely different builds does not reliably predict WNS margin,
  even in the direction of the correlation — a higher-occupancy build can close with a dramatically
  BETTER margin than a lower-occupancy one.** This project has now seen the relationship point both
  ways enough times that "higher occupancy = tighter/riskier timing" should not be assumed as a
  default heuristic, even loosely: the DW-raster round closed at 77.52% LUT with only +0.021ns; the
  gmemmeta-elimination round closed at 59.25% with +0.272ns (fits the naive heuristic); but the
  MAC_PD=2->4 round (2026-09-05, ZHR-92) closed at 80.79% LUT — the highest occupancy this project has
  ever closed timing at — with **+0.338785ns, the best margin on this project's entire timing
  history**, beating both the 67.20%-LUT MAC_PD=2 build (+0.094ns) and the 61.40%-LUT MAC_PD=1
  baseline (+0.134ns) it descended from. Consistent with the HLS-binding-pragma finding directly
  above (resource totals and timing outcome are separate levers, steered by RTL structure/placement,
  not just aggregate occupancy) but distinct in scope: that finding was about ONE pragma choice on
  otherwise-identical resource totals; this one is about genuinely different architectural
  configurations (different MAC_PD values) with genuinely different, substantial resource totals,
  still failing to predict WNS direction from occupancy alone. **Use LUT% as a rough feasibility gate
  (a DRC-level "will this even fit" check, and this project's own pblock-sizing discipline still
  applies), not as a timing-margin predictor — only a real P&R run answers the timing question,
  regardless of how the occupancy percentage compares to a prior build's.**
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

## Current deployed baseline (updated 2026-09-14, later -- supersedes every earlier baseline reference below)

**`mac_array_a3_rowread` is now the deployed baseline**, replacing `mac_array_a3_rowburst` (~790ms /
DW 224.6ms / WNS +0.312ns, same day). Full network **~679ms** (679.35 / 679.43ms over two runs,
per-entry sums 659.9 / 659.4), **-14%**; DW 224.6 -> **115.5ms (-48.6%)**. Cumulative on this
latency line: 6,050ms -> ~679ms, **-88.8%**. Archive + writeup:
`vivado_impl/bitstream_archive/mac_array_a3_rowread_2026-09-14/README.txt`.

**What changed** (`DWR_ROWREAD`, commit `c659a97`, default ON since this round via
`dw_raster_layer.h` -- `DWR_ROWREAD_OFF` reverts; mutually exclusive with `DWR_INPUT_BURST`):
the input-side twin of `DWR_ROWBURST` -- one `read_request(row_addr, w_in/4)` per in-image row
BEFORE `dwr_produce`'s COL loop, the word-packed `read()` INSIDE the pipelined loop (COL II=1,
iteration latency 12 -> 3), reads staying overlapped with consume through the DATAFLOW pair. NOT
`DWR_INPUT_BURST`'s serial per-channel prefetch (additive, measured +43ms). Port: `dw_in_burst`,
left behind by that rejected round with its driver register already wired -- zero interface
change (see the infrastructure-reuse rule in the working-method section). Alignment checked
first: all 79,872 real DW input row starts mod4==0; padding never touches addressing. Step-1
schedule probe before implementing: readreq only in the ROW body, read() only in `Pipeline_COL`,
COL II=1 -- and ZERO II violations design-wide, the first time on this line. Default-flip
verified: flag-less csynth totals bit-identical (246/29/37,979/71,873, `hw.h` identical),
flag-less csim 5/5+4/4+8/8+4/4.

Real P&R (route_design alone, NO phys_opt): **WNS +0.112692ns** (rowburst +0.312 -- a placement
roll on the SAME worst structure, `gmem_w` load buffer -> DW consume gather, 12 levels, 96%
route; not the new read code); LUT 44,422/53,200 (83.50%, +179 vs rowburst -- isolated said
-1,005, opposite sign); BRAM 107 (flat); DSP 32/220 (14.55%, -4 as isolated).

Board (2026-09-14, pre-registered order, 30s timeouts, golden untouched): entry5_dw FIRST, 3
runs, byte-exact, 14.2 -> 5.6ms (-60%; 38.9ms three rounds ago); SE ops 75/77/79/80 byte-exact;
controls entry3/entry0_gelu/entry10_add byte-exact; full network 82/82 x2, all 7 checkpoint
files MD5-identical; DWCONV 115.56 / 115.51 (-48.6%, pre-registered 100-150 = "input-side
handshake WAS the bottleneck, same mechanism as the output side, same fix"), PWCONV 495.3 (flat),
GELU/ADD/SE flat; ONNX cosine EXACT 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811. Register map
unchanged. Operator split now: **PW 73%**, DW 17%, GELU 3.4%, ADD 1.9%, SE 1.9%.

**Refit on this run (R^2 0.88): DW = 1.29 cycles/input-pixel + 1.30 cycles/output + 886/channel
= 46 + 27 + 39ms** (history: dwob 4.72/7.45/160 -> rowburst 6.08/0.30/90 -> here). The pixel
term is at the II=1 floor (35.5ms). What is left of DW is three small terms, the per-CHANNEL one
now the largest (~886 cycles/channel: `dwr_consume`'s pre-loop weight/bias loads as `fpg*(2+K^2)`
individual DRAM reads, DATAFLOW start, the per-row loop overheads folded in). DW at 17% of the
network is no longer where the time is -- **PW (73%) is the target now.**
**PW DECOMPOSED, 2026-09-14 (step 0 + step 1's schedule check, no build), the same way as DW:**
schedule check first -- `pw_flat_pipeline_impl_*_Pipeline_PW_FLAT` has `writereq` at ST_9, `write`
at ST_10 and a 5-stage `writeresp` at ST_11-15, ALL inside the II=1 `PW_FLAT` iteration -- the
IDENTICAL stage numbers DW's `CROW_CCOL` had before ROWBURST: every 4-byte output word waits for
its own B response inside the pipeline. (`ROW_READ` is already the right shape: requests in
`run_layer`'s `ROW_READ_CH` loop, reads inside `ROW_READ_FILL`.) Fit on the `rowread` run, all 26
PW layers, physical terms (R^2 0.992; 0.998 with weight bytes): **PW 495.1ms = 1.35 cycles per
`PW_FLAT` iteration (177ms; floor 1.0 = 131ms) + 18.0 cycles per output WORD WRITE (907,056
writes, 163ms) + 83.5 cycles per `ROW_READ` REQUEST (179,904 requests, 150ms)** (+0.89
cycles/weight byte, 26ms, when added). Two handshake terms = ~310ms = 63% of PW. Measured/floor
per layer 2.9-6.6x (the two SE fc layers 8.8x/32x, tiny). This is the "73.5% unexplained" from
the 1,806ms era re-measured on the current architecture -- it is now two named mechanisms.
**Candidate fixes (not built), both keeping the data op inside the pipeline:** (a) WRITEOUT: the
4 rows of one tile are NOT contiguous (w_out apart), so a DW-style row burst does not apply
directly -- but the stall is the RESPONSE wait, not the request count: defer `write_response()`
to a later iteration (pop the previous-but-one ot's 4 responses during this ot's writeout rows;
<=16 outstanding respected; drain after the loop) -- keeps request+write where they are, ~0
stall for every real cin (the gap is >= 2 x (n_cbase*8+16) >= 64 cycles); (b) ROW_READ: 83
cycles per request for only w/4 (2-16) words is request latency + fill/drain per request --
issue the 4 `rr` requests (or a ci-group's 16) back-to-back before the fill loops so the
latencies overlap (NUM_READ_OUTSTANDING=16). Rough ceiling: 163 -> ~30ms and 150 -> ~50ms,
PW ~495 -> ~260ms, network ~679 -> ~450ms. Each is its own step-1 probe first.

## Prior deployed baseline (superseded 2026-09-14, kept for history)

**`mac_array_a3_rowburst` was the deployed baseline for part of 2026-09-14**, replacing `mac_array_a3_dwob` (~898ms /
DW 329.0ms / WNS +0.172ns, deployed 2026-09-13). Full network **~790ms** (791.96 / 789.26ms over
two runs, per-entry sums 769.97 / 769.28), **-12%**; DW 329.0 -> **224.7ms (-31.7%)**. Cumulative
on this latency line: 6,050ms -> ~790ms, **-86.9%**. Archive + full writeup:
`vivado_impl/bitstream_archive/mac_array_a3_rowburst_2026-09-14/README.txt`.

**What changed** (`DWR_ROWBURST`, commit `5079957`, default ON since this round via
`dw_raster_layer.h` -- `DWR_ROWBURST_OFF` reverts; requires `DW_OUTPUT_BURST`): the packed DW
output write's request/response moved from once-per-word-inside-the-iteration (each write waited
~30 cycles for its own B response inside the II=2 pipeline: 522,240 waits, ~156ms) to once per
lane-row outside the pipelined loop; lane 1's words go through a per-row buffer drained after lane
0's burst (AXI data order = AW order). Side effect: one bus write per iteration -> `CROW_CCOL`
achieved **II 2 -> 1** (the `200-880` was a property of the lane structure, not the hardware --
see the corrected `200-880` rule above). Verified in the schedule BEFORE implementing (step 1
probe: writereq/writeresp out of the iteration body, write(word) still inside) -- not the
`PW_WRITEOUT_FLUSH` shape. Default-flip verified: flag-less csynth totals bit-identical to the
tested build (246/33/38,792/72,878, CCOL II=1, `hw.h` identical), flag-less csim 5/5+4/4+8/8+4/4.

Real P&R (route_design alone, NO phys_opt): **WNS +0.311772ns** (best since macpd4's +0.339);
LUT 44,243/53,200 (83.16%, +163 vs dwob -- isolated said +788); BRAM 107 (flat); DSP 36/220
(16.36%, -10, exactly as isolated: lane bases once per channel replaced the per-write
`co*out_ch_stride`). Worst path: `gmem_w` load buffer -> DW gather (10 levels, 95% route) -- the
route-dominated population; `dwr_produce`'s carry chain left the top-5.

Board (2026-09-14, pre-registered order, 30s timeouts): entry5_dw FIRST, 3 runs, byte-exact,
24.8 -> 14.2ms (-43%); SE ops 75/77/79/80 byte-exact; controls entry3/entry0_gelu/entry10_add
byte-exact at their usual times; full network 82/82 x2, all 7 checkpoint files MD5-identical;
DWCONV 224.74 / 224.58 (-31.7%), PWCONV 496.3 (+0.2%), GELU/ADD/SE flat; ONNX cosine EXACT
0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811. Register map unchanged. Golden untouched.
Operator split now: **PW 63%**, DW 28%, GELU 2.9%, ADD 1.7%, SE 1.6%.

**The landing point's information (the round's most important number): DW landed at 224.7, the
"II gain NOT realised" end of the pre-registered 182-217.** Refit on this run (R^2 0.995): DW =
**6.08 cycles/input-pixel + 0.30 cycles/output + 90/channel** (dwob: 4.72 / 7.45 / 160). The
output term collapsed 156 -> 6ms -- the mechanism did exactly what it was designed to do -- and the
per-pixel term is now 216 of DW's 225ms: ~6 cycles per input pixel, UNIFORM across stride-1 and
stride-2 layers (5.7-7.0; the tiny fpg=2 s1 layer 74 at 11.2 is row-overhead-dominated), where
consume at II=1 needs ~1.3-3.6. **`dwr_produce`'s plain per-pixel `in_base[]` AXI read is now THE
DW bottleneck, definitively -- exposed by roughly (6.08 - 1) x 3.55M pixels = ~180ms.** This is the
third time the "did the II saving materialise per layer" method decided a question on this line,
and it reverses the 2026-09-13 DWR_INPUT_BURST verdict's premise: that verdict ("produce hidden,
don't reopen") was correct at consume II=2 and is now void at II=1. The input side needs its own
handshake-count treatment -- the SAME shape as this round (row-granularity `read_request` outside
the pipelined loop, word-packed `read()`s inside it, so the reads stay overlapped with consume
through the DATAFLOW pair), NOT `DWR_INPUT_BURST`'s serial per-channel prefetch (which pulls the
read out of the overlap and was measured at +43ms of additive cost). Not started.

## Prior deployed baseline (superseded 2026-09-14, kept for history)

**`mac_array_a3_dwob` was the deployed baseline from 2026-09-13 to 2026-09-14**, replacing `mac_array_a3_sohoist` (1,095.36ms /
82.81% LUT / WNS +0.138ns route-only, deployed 2026-09-12). Full network **~898ms** (903.14 /
893.46ms over two runs; per-entry sums 873.24 / 873.11, identical -- the PL-total spread is
inter-entry host jitter), **-18%**, DW 531.13 -> **329.0ms (-38.0%)**. Cumulative on this latency
line: 6,050ms -> ~898ms, **-85.2%**. See `vivado_impl/bitstream_archive/mac_array_a3_dwob_
2026-09-13/README.txt` for the full writeup.

**What changed** (two source changes over sohoist, both now the DEFAULT build):
1. `SHARED_MUL_ARMS` (commit `83b4cdb`): the seven remaining call sites on the top-level shared
   32x32 multiplier removed (accumulators + narrow-typed multiplies); `mul_32s_32s_32_2_1` now
   exists only inside DW's own `dwr_consume3`. Alone: WNS flat (+0.128), sink gone from the top-10.
2. `DW_OUTPUT_BURST` (port-reuse form; commit `dfd309f`, default flipped ON this round via
   `dw_raster_layer.h` -- `DW_OUTPUT_BURST_OFF` restores the old writeout): `dwr_consume`'s
   `CROW_CCOL` achieved II 8 -> 2 by packing each lane's 4 output bytes into one `ap_uint<32>`
   write on PW_FLAT's existing `out_burst`. Failed real P&R twice on the sink that (1) removed;
   on top of (1) it closed at **+0.172093ns route_design alone** -- the "different coin" outcome.
   The `wbuf` partition pragma rides along under the same macro; on its own it never changed II.
   Default-flip verified: flag-less csynth totals bit-identical to the tested build (245 BRAM_18K /
   43 DSP / 39,299 FF / 72,090 LUT, `CROW_CCOL` II=2, `hw.h` identical), flag-less csim 5/5 + 4/4 +
   8/8 + 4/4.

Real P&R (route_design alone, NO phys_opt): WNS +0.172093ns; LUT 44,080/53,200 (82.86%); BRAM
106.5/140 (76.07%); DSP 46/220 (20.91%). Worst path: `gmem_act` load-unit read-data buffer
(9 levels, 77% route) -- a member of the route-dominated population (+0.17..+0.45 at 10ns) that
is this design's real floor now that the shared-multiplier sink is gone. The real LUT (44,080)
was forecast at ~44,300 from the DW mechanism's own two prior isolated/real deltas -- see the
"same mechanism's historical ratio" rule in the working-method section.

Board (2026-09-13, pre-registered order): entry5_dw FIRST, 3 runs, byte-exact, 38.9 -> 24.7-24.95ms
(-36%); SE ops 75/77/79/80 byte-exact vs the Python reference; controls entry3 PW / entry0 GELU /
entry10 ADD byte-exact at their usual times; full network 82/82 x2, all 7 checkpoint files
MD5-identical across runs; per-operator DWCONV 329.0 (-38.0%, pre-registered 250-400), PWCONV
495.3 (-0.09%), GELU 22.81, ADD 13.04, SE 12.93 (all flat); ONNX cosine EXACT
0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811. Register map unchanged (last register `DW_IN_BURST`
0x100); `*_sohoist` board binaries remain valid. Golden image untouched (md5 `7ee26f67...`).
Operator split now: PW 55%, DW 37%, GELU 2.5%, ADD 1.5%, SE 1.4% -- **PW is the single largest
operator again.**

## Prior deployed baseline (superseded 2026-09-13, kept for history)

**`mac_array_a3_sohoist` was the deployed baseline from 2026-09-12 to 2026-09-13**, replacing `mac_array_a3_elemwise_burst`
(1,099.77ms / 83.22% LUT / WNS +0.017ns via phys_opt only, deployed 2026-09-07). This promotion is a
**timing-margin** promotion, not a latency one: full-network time is flat by design (1,095.36 /
1,094.50ms over two runs, -0.4%), and what changed is that the design now closes timing on
`route_design` ALONE at **WNS +0.138388ns** -- the prior baseline was -0.167ns route-only and only
reached +0.017ns through `phys_opt_design`, leaving no headroom for any subsequent change (see the
"IMPORTANT CHARACTERIZATION" note in the superseded section below, which this promotion resolves).
See `vivado_impl/bitstream_archive/mac_array_a3_sohoist_2026-09-12/README.txt` for the full writeup.

**What changed** (`SCALAR_OP_SIZE_HOIST`, commit `fc6f6e0`): the six scalar ops' element-count
multiplies (10 op_type-gated call sites feeding the top-level shared 32x32 multiplier -- the
critical-path sink of every thin-margin build on this line) replaced by two unconditional multiplies
in `mac_array_top` (`scalar_hw`, `scalar_total`) passed in as `int` parameters. Binding DB: top-level
Multiplier opset 8 op_type-predicated ops -> 2 `Predicate=true`; shared multiplier's operand mux 4
arms -> 2; one 32x32 multiplier instance gone. Critical path kept its sink and source shape but its
data path dropped 8.707 -> 8.251ns, BOTH logic (mux LUT6 -> LUT4) and route -- structural, not a
placement roll. Also in this build: `DW_OUTPUT_BURST` gated OFF by default (default DW path
source-identical to the prior baseline's).

Real P&R (route_design alone, NO phys_opt): WNS +0.138388ns; LUT 44,054/53,200 (82.81%, -218);
BRAM 107/140 (76.43%, flat); DSP 56/220 (25.45%, -3).

Board (2026-09-12): SE ops entry75/77/79/80 (GAP/RELU/SIGMOID/SCALE) byte-exact against an
INDEPENDENT Python reference on real `desc_all.bin` descriptors (`tools/gen_scalar_ops_csim_bundle.py`
-> `board_test_scalar_ops/`), first checked as a control on the prior baseline bitstream (all pass)
and then on this one (all pass) -- the first board verification of these four ops against anything
other than a csim dump. Controls entry3/entry5_dw/entry0_gelu/entry10_add all byte-exact. Full
network 82/82, two runs, all 7 checkpoint files MD5-identical across runs; per-operator DWCONV
531.13 / PWCONV 495.77 / GELU 22.90 / ADD 13.04 / SE 12.95ms, every family within +-0.2% of the
prior baseline; ONNX cosine EXACT: 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811. Register map
unchanged (DW_IN_BURST at 0x100 remains the last register). Board binaries rebuilt from current
source this round (`*_sohoist`). Golden image untouched. New csim testbench
`scalar_ops_real_desc_tb.cpp` (`run_csim_scalar_ops.tcl`, 4/4) closes the coverage gap that
existed for these four ops on this architecture; `tools/decompose_full_network_log.py` now does
the per-operator decomposition from a full-network log (all 82 entries, not the log's own top-10).

**Pre-registered next round: re-enable `DW_OUTPUT_BURST` on top of this build, single variable.**
It landed at -0.418ns route-only on the old baseline (which itself was at -0.167), i.e. it needs
roughly 0.25ns more than the old baseline had; this build has +0.138. The +0.305ns from one
call-site cleanup says the sink's mux structure had room -- another cleanup of the same kind on
whatever call sites remain in that mux (currently `run_layer`'s own arm and the unconditional
`w_in*h_in`) is the candidate for the rest, before or alongside the DW_OUTPUT_BURST re-test.

## Prior deployed baseline (superseded 2026-09-12, kept for history)

**`mac_array_a3_elemwise_burst` was the deployed baseline from 2026-09-07 to 2026-09-12**, replacing `mac_array_a3_macpd4`
(1,522.39ms/80.79% LUT/WNS+0.339ns, deployed 2026-09-05). Promotes the ELEMWISE_BURST mechanism
(chunked `hls::burst_maxi<ap_uint<32>>` reads/writes for `run_gelu`/`run_add`, replacing plain un-
bursted pointer accesses) -- see ZHR-92's own round writeup for the full diagnostic chain (the
operator decomposition that found GELU/ADD were 8.60x/16.57x a naive per-element floor, the csynth
codegen crash and its fix, the real P&R WNS journey, and the two real pre-existing bugs the board
deployment surfaced and fixed).

**What changed**: `run_gelu`/`run_add` now use two new burst ports (`elemwise_in_burst`/
`elemwise_out_burst`, `ap_uint<32>`-typed, 4 lanes unrolled per word) sharing the existing `gmem_act`
bundle -- no new AXI master, no BD change. `run_add`'s own two-source case reads sequentially (buffer
`in_off`'s chunk, then read `in2_off`'s) rather than simultaneously, which incidentally also resolved
its pre-existing `II=2` port-contention violation (a bonus, not pre-registered).

Real P&R: **route_design alone did NOT close (WNS=-0.166790ns)** -- the critical path was confirmed
identical to `macpd4`'s own pre-existing `mul_32s_32s_32_2_1` mechanism (resource-pressure-induced
degradation, not a new bottleneck from the burst rewrite), so a single `phys_opt_design` pass (not a
pblock, not directive rotation) was applied, recovering **WNS=+0.017ns**.

**IMPORTANT CHARACTERIZATION, required before citing this build's own margin in any future round:**
this baseline's margin comes ENTIRELY from `phys_opt_design`, not from `route_design` alone (which is
still negative at -0.167ns on the identical netlist). This is qualitatively different from every
prior deployed baseline on this line (macpd2, macpd4, pw_wchunk, etc.), which all closed on
`route_design` alone with real margin to spare. **+0.017ns means the NEXT change to this design has
essentially zero headroom -- deployable, but with no room to move.** Any future round that modifies
this design should re-run BOTH `route_design` alone AND `phys_opt_design` fresh, not assume the same
`phys_opt_design` recovery will reproduce on a changed netlist.

Real utilization: LUT 44,272/53,200 (83.22%, +2.43pp over macpd4's 80.79%), BRAM 107/140 tiles
(76.43%, +0.72pp), DSP 59/220 (26.82%, +4.09pp). Isolated csynth had projected ~84.5% LUT (using
macpd4's own isolated-to-real ratio) -- real came in lower, the project's own now-well-established
pattern of isolated-vs-real divergence having no consistent direction.

Board: full network **1,099.77ms**, down from 1,522.39ms (**-27.8%**, the SECOND-largest single-round
win on this whole latency-optimization line, behind only PW weight-residency's own -41.9%).
GELU 328.47ms->22.93ms (-93.0%), ADD 138.42ms->13.06ms (-90.6%), PW 495.88ms->496.08ms and DW
531.51ms->531.20ms both unchanged (no regression). Cumulative from this line's original 6,050ms
starting point: **-81.8%**. Six-checkpoint correctness verified via cosine similarity against the
untouched ONNX float32 reference: **0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811** -- EXACT match to
the project's own long-established figures. Register map changed (two new burst ports added, each
needing its own AXI-Lite base-address register programmed by the host -- see the two real bugs this
round's board deployment fixed, below) -- **any ARM-side binary built before this round's own driver
fix (`mac_array_driver.h`'s `MAC_ELEMWISE_IN_BURST_LO/HI`/`MAC_ELEMWISE_OUT_BURST_LO/HI`) will hang or
silently produce wrong output on any real GELU/ADD dispatch** -- always rebuild driver/test harnesses
from current source before using them against this build.

**Two real, pre-existing bugs this round's board deployment surfaced and fixed (neither in the
ELEMWISE_BURST HLS logic itself):**
1. `tools/build_single_op_test_entry0_gelu.py`/`entry10_add.py` were stuck at 27-field descriptors
   (`MacLayerDesc` grew to 28 fields on 2026-08-31, `use_wide_path` appended) -- never updated, and
   their bundles had never actually been built or run before this round (confirmed via the board's
   own directory listing). **A sweep of every other `build_*.py` bundle generator's own asserted field
   count found 18 MORE scripts still stuck at 27 fields** (DW/PW probe builders, several single-op
   entry builders for RELU/SIGMOID/SCALE/GAP/etc.) -- likely harmless in PRACTICE (`use_wide_path` is
   confirmed dead code in the active dispatch path, and many of these bundles were already
   successfully board-tested in earlier rounds despite the staleness, which is itself evidence their
   own dead field never mattered) but a real, verified latent-risk pattern, not yet fixed for any of
   the 18. Confirmed NOT load-bearing for this round's own bug: fixing `entry10_add`'s own desc.bin
   alone (before the register-wiring fix below) still produced 100%-poison wrong output -- the field-
   count fix was a real, worthwhile correctness cleanup, but not what actually resolved the hang or
   the wrong output.
2. **The actual proximate cause of both real symptoms**: `elemwise_in_burst`/`elemwise_out_burst`'s
   own AXI-Lite base-address registers (found at 0xe8/0xec and 0xf4/0xf8 in the real exported IP's own
   `xmac_array_top_hw.h`) were never wired into the ARM driver. This is the THIRD confirmed instance
   of this project's "sharing an m_axi bundle does NOT mean sharing a control register" trap (after
   `out_burst` and `in_burst`'s own identical history) -- and the first instance where the risk was
   explicitly named in the code's OWN header comment in advance ("own control register, not yet wired
   into mac_array_driver.c... a P&R-stage TODO, tracked, not silently deferred") and STILL shipped
   unfixed to a real board round. **New standing lesson: flagging a risk in a comment is not the same
   as handling it -- this project now has two confirmed instances of a self-identified, explicitly-
   named risk reaching a real board deployment anyway (this one, and the `in_burst`/`out_burst` gap
   this same comment references as its own precedent).** An explicit-risk comment should be treated as
   a checklist item to close before board deployment, not a substitute for closing it. csim could not
   catch this (no register-address concept) -- it manifested exactly as this project's own established
   precedent for this failure class predicts: unprogrammed `elemwise_in_burst` caused a genuine AXI bus
   hang on the largest real GELU dispatch (786,432 elements); unprogrammed `elemwise_out_burst` caused
   output 100% identical to the pre-dispatch poison pattern despite `ap_done`/`out_written=1` firing
   normally -- the same "IP completes, output silently not written" signature this project's own
   `mac_array_tb.cpp` header comment already names as a known defect class. Fixed by adding the
   register writes to all three real ARM-side call sites (`mac_array_single_op_test.c`, `_add.c`,
   `mac_array_full_network_test.c` -- grepped for every real caller, per this project's own established
   discipline for interface changes).

**`phys_opt_design`'s own applicability boundary, now with 3 data points**: recovers a real negative
WNS successfully when the route_design-alone violation is within roughly **-0.2ns** (dummy4th:
-0.133->+0.013ns; this round: -0.167->+0.017ns; an earlier round also within this range) -- but does
NOT single-handedly recover a violation at the -2.264ns magnitude (the PW_WCHUNK shared-multiplier
regression, which needed an actual source-level fix, not just `phys_opt_design`). **This boundary
(~-0.2ns) can now be used directly as a rough decision rule for whether a single `phys_opt_design`
pass is worth attempting on a fresh violation, without re-deriving it from scratch each time** -- a
violation deeper than roughly -0.3 to -0.5ns should be treated as needing a real source-level
diagnosis first, not a `phys_opt_design` attempt on its own.

## Prior deployed baseline (superseded 2026-09-07, kept for history)

**`mac_array_a3_macpd4` was the deployed baseline from 2026-09-05 to 2026-09-07**, replacing
`mac_array_a3_macpd2`
(1,626.70ms/67.20% LUT/WNS+0.094ns, deployed 2026-09-04). Second step of the MAC_PD-widening line,
same day as MAC_PD=2's own promotion -- continuing to MAC_PD=4 was justified specifically because
the BRAM dual-port question flagged (but never reached) at MAC_PD=2 turned out to resolve itself:
HLS auto-inferred `cyclic factor=2` partitioning on `pw_weight_cache` via its own pipeline-scheduling
pass (`[HLS 214-270] Inferring pragma 'array_partition type=cyclic factor=2 dim=1'... due to pipeline
pragma`) -- 2 banks x 2 native BRAM read ports = 4 simultaneous reads, exactly matching MAC_PD=4's
need, with ZERO manual partitioning pragma required. `PW_FLAT` achieved II=1 immediately, no
diagnostic violation at all for this mechanism.

**What changed**: `MAC_PD` 2->4. No source change beyond the macro -- the same compile-time-
eliminated uncached-fallback fix from the MAC_PD=2 round already covers this (the dead branch was
never MAC_PD-specific).

Isolated csynth (pre-registered stop condition check, per this round's own plan): LUT jumped
56,358->70,661 (+25.4%) over MAC_PD=2's own isolated baseline -- projected via the MAC_PD=2 round's
own measured real/isolated ratio (0.634) to ~84.3% real LUT, above every prior successful closure on
this line (77.52% was the previous high-water mark, with only +0.021ns margin). Per this round's own
pre-registered stop condition ("if LUT jumps a lot, stop and report before committing to P&R"),
this was reported as a decision point rather than run automatically -- decided to spend the real P&R
run anyway specifically because MAC_PD=2's own isolated-to-real divergence had gone the FAVORABLE
direction (56,358 isolated -> 35,751 real), and this project's own repeated finding is that the
divergence direction is not predictable in advance.

**Real P&R came back the best-margin build on this entire session's timing history, despite the
highest LUT occupancy**: WNS=+0.338785ns (vs MAC_PD=2's +0.094463ns and MAC_PD=1's +0.134ns -- both
LOWER margins at LOWER LUT occupancy), LUT 42,980/53,200 (80.79%, +13.59pp over MAC_PD=2's 67.20%,
the isolated projection (84.3%) overshot but the real number is still the highest LUT occupancy this
project has ever closed timing at), BRAM 106/140 tiles (75.71%, +24 tiles over 58.57%), DSP 50/220
(22.73%, exactly unchanged across all three MAC_PD values -- confirms yet again the whole MAC
datapath is LUT-inferred, zero DSP). **This is a second, independent data point (after the DW-raster
round's own 77.52%-at-+0.021ns and the gmemmeta-elim round's 59.25%-at-+0.272ns) that real timing
margin does not correlate simply with LUT occupancy percentage on this design -- the specific
critical-path structure matters more than the aggregate occupancy number.** Do not use LUT% alone to
predict WNS direction; a higher-occupancy build closing with a dramatically better margin than a
lower-occupancy one is now a repeated pattern on this project, not a fluke.

Board: PL-side full-network total **1,522.39ms**, down from 1,626.70ms (**-6.41%** further;
cumulative from the MAC_PD-widening line's own start, MAC_PD=1's 1,806.45ms: **-15.73%**; cumulative
from this whole latency-optimization line's original 6,050ms starting point: **-74.8%**). Single-op
byte-exact vs csim, all PASS: entry3 (PW-only) 19.11ms(MAC_PD=2 mean)->17.30-17.36ms mean ~17.32ms
(**-9.4%** further, matching the pre-registered ~7-8% model prediction for the 2->4 step -- PW's
compute share keeps halving with each doubling of parallelism, so each step's own marginal benefit
shrinks, exactly as pre-registered); entry5_dw (DW-only) 38.82/38.87/38.88ms across all three MAC_PD
values (1/2/4) -- UNAFFECTED, within noise, at every step, confirming the whole widening line is
PW-specific and DW's own already-generic-in-MAC_PD datapath never engages with any of this. Full-
network six-checkpoint correctness verified via cosine similarity against the untouched ONNX float32
reference: **0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811** -- EXACT match to the project's own
long-established figures, at every MAC_PD value tested (1, 2, 4) -- confirms the whole widening line
preserves numeric semantics exactly. Register map unchanged from `mac_array_a3_macpd2`.

**Diminishing returns, pre-registered before this round ran and confirmed by it**: MAC_PD 1->2 saved
~15.7% of PW's own time; 2->4 saved a further ~9.4% -- each doubling of parallelism recovers roughly
half of what the previous doubling did, exactly as the "PW's compute share keeps halving" model
predicts. MAC_PD=8 would be expected to save only ~4-5% more of PW's time, for a LUT cost that (by
the same roughly-doubling pattern seen 2->4) could push real occupancy well past 90%, a resource
region this project has never closed timing in. **MAC_PD=8 is very unlikely to be worth attempting
-- this is a pre-registered judgment, not yet tested, recorded here so a future round doesn't have to
re-derive the diminishing-returns argument from scratch before deciding whether to spend a round on
it.**

## Prior deployed baseline (superseded 2026-09-05, kept for history)

**`mac_array_a3_macpd2` was the deployed baseline from 2026-09-04 to 2026-09-05**, replacing
`mac_array_a3_pw_wchunk` (1,806.45ms/61.40% LUT/WNS+0.134ns, deployed 2026-09-03). This re-opens and
closes the MAC_PD-expansion line that was CLOSED on 2026-08-31 (see the hard-stop-list bullet above)
-- that closure's
own premise (`gmem_w`'s single AXI port can't service >1 bus request/iteration) was measured on the
PRE-weight-hoist architecture; weight caching (`pw_weight_cache`/`PW_WCHUNK`, deployed since
2026-09-02/03) removed the real bandwidth demand the closure was about, but the dead uncached
fallback arm was left as a runtime `if(pw_cached)` branch that the MAC_PD=1->2 unroll re-triggered
the SAME diagnostic against for an unrelated, scheduler-artifact reason -- see the hard-stop-list
bullet's own 2026-09-04 refinement and ZHR-92 (Linear) for the full diagnostic chain.

**What changed**: `MAC_PD` 1->2 (PW's Cin reduction-chunk size, doubling the number of channels
`PW_FLAT` processes per cycle). The dead direct-DRAM-read fallback arm inside `pw_flat_pipeline_impl`
(unreachable on every real PW layer since PW_WCHUNK guarantees 100% cache coverage, `d.cin <=
PW_WEIGHT_CACHE_ELEMS` always true for real `cin<=1152` vs `cache=147,456`) is now a compile-time-
constant `if(true)` by default, structurally removing it from the scheduler's view instead of leaving
it as a runtime branch HLS must conservatively schedule for; the old runtime-gated form is preserved,
unused by default, behind `PW_ALLOW_UNCACHED_FALLBACK` per this project's own dead-but-kept-fallback
convention. Confirmed via isolated csynth before touching P&R: `PW_FLAT` achieved II regresses to 2
with the runtime branch, recovers to 1 with the dead arm compile-time-eliminated -- root cause is the
MAC_PD-unrolled `dd` loop duplicating the whole `if/else` (including the dead `gmem_w` arm) across 2
simultaneous lanes, not real weight-read bandwidth demand.

Real P&R (route_design alone, no phys_opt needed): **WNS +0.094463ns** (closed, down from +0.134ns --
thinner margin, still positive), **LUT 35,751/53,200 (67.20%)**, up +3,087/+5.80pp from 61.40%,
**BRAM 82/140 tiles (58.57%)**, up +8 tiles from 52.86%. **DSP 50/220 (22.73%)** -- exactly unchanged.

Board: PL-side full-network total **1,626.70ms**, down from 1,806.45ms (**-9.95%**; cumulative from
this whole latency-optimization line's original 6,050ms starting point: **-73.1%**). Single-op
byte-exact vs csim: entry3 (PW-only, degenerate path) 22.69ms->18.41-19.46ms (**-14.1% to -18.7%**,
mean ~15.7%, matching the pre-registered model -- PW's compute share of real board time (28%, from
the earlier cosim decomposition) doubling in parallelism predicts ~14% of PW's own time saved, and
measured landed close to that once properly scoped to a PW-only shape); entry5_dw (DW-only) 38.82ms
->38.87ms, **unaffected (within noise)** -- confirms MAC_PD=2's win is PW-specific, DW's own datapath
(already `complete dim=0`-partitioned, generic in MAC_PD) neither gains nor regresses. The full-network
aggregate (-9.95%) sits between PW-only's -15.7% and DW's ~0% because the network mixes both --
consistent with PW being roughly 63% of full-network time by this arithmetic, not evidence of
serialization eating into the model's prediction. Full-network six-checkpoint correctness (primary
judge per this project's own standing process rule) verified via cosine similarity against the
untouched ONNX float32 reference: **0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811** for
stage1/stage2/stage3/stage4/finaldw/se -- EXACT match to the project's own long-established figures,
confirming MAC_PD=2 does not change numeric semantics. Register map is UNCHANGED from
`mac_array_a3_pw_wchunk` (MAC_PD is an internal datapath-width parameter, not a register) -- existing
ARM-side binaries remain valid, no rebuild needed. csim was not separately re-run this round (only
isolated csynth + real board); real hardware correctness (byte-exact single-op + exact ONNX cosine
match) was treated as sufficient given the scope, but a formal csim pass is still owed if this build
is revisited.

Not investigated this round: whether MAC_PD=4 is viable now that the dead-branch mechanism is
understood (the BRAM dual-port question raised at the start of this round -- `pw_weight_cache` is a
plain, unpartitioned array with 2 native read ports, matching MAC_PD=2 exactly but insufficient for
MAC_PD=4 without partitioning -- was never actually reached, since `gmem_w`'s dead-branch artifact
was the binding constraint at MAC_PD=2, not BRAM ports). 150MHz was deliberately deferred to a
separate round per this round's own pre-registration (avoiding a confounded two-variable change).

## Prior deployed baseline (superseded 2026-09-04, kept for history)

**`mac_array_a3_pw_wchunk` was the deployed baseline from 2026-09-03 to 2026-09-04**, replacing
`mac_array_a3_pw_weight_hoist`
(2,111.27ms/60.06% LUT/WNS+0.153ns, deployed 2026-09-02). This closes out the 351.44ms fallback-layer
opportunity that `pw_weight_hoist` itself flagged but did not capture -- see ZHR-63's mainline summary
and ZHR-92's own round-by-round history (including a 3-round real-P&R shared-multiplier regression
chase) for how it was reached, `vivado_impl/bitstream_archive/mac_array_a3_pw_wchunk_2026-09-03/
README.txt` for the full technical writeup.

**What changed**: the prior baseline's 144KB `pw_weight_cache` covered only 22 of 26 real PW layers;
the 4 layers exceeding 144KB (`layer_0043/44/47/48_pwconv`) fell back to a direct-DRAM-read path
board-measured (2026-09-02) to cost 351.44ms combined (78.7% of their own 446.42ms) in redundant
weight reads. This round replaces the single-cutoff cache with `PW_WCHUNK`: an outer loop (wrapping
the entire `(rt,colt)` spatial sweep in `run_layer`) that loads the same 144KB cache in chunks instead
of one all-or-nothing load, so every real PW layer is now served from on-chip cache -- the direct-read
fallback is dead code on any constructible shape (kept, not deleted). Chunk boundaries are always
per-ot (confirmed against real descriptors: L43/44 split into 3 even chunks, L47/48 into 3 uneven
chunks, 384+384+192/153+153+78).

**Real P&R timing history is the headline finding of this round**: the first two chunking attempts
regressed real timing hard (WNS -2.264415ns, then -1.127835ns after a partial fix), both traced via
the actual critical-path report (not guessed) to this project's own previously-documented "shared
multiplier bound to FSM state" mechanism -- but via two DIFFERENT specific causes exposed in sequence
by `PW_WCHUNK`'s own new outer loop level restructuring `run_layer`'s FSM. Both fixed the same way
(hoist the offending multiply to a loop-carried accumulator in `PW_WCHUNK`, stepped by each chunk's
REAL clamped count, not the nominal chunk size) -- **third attempt: WNS=+0.133715ns, closed.** See
CLAUDE.md's own refined shared-multiplier entry (below, in the working-method section) for the general
lesson this produced.

Real P&R (route_design alone, no phys_opt needed): **WNS +0.133715ns** (closed, down from +0.153200ns
but still positive), **LUT 32,664/53,200 (61.40%)**, up +1.34pp from 60.06%, **BRAM 74/140 tiles
(52.86%)** -- exactly unchanged from the prior baseline. **DSP 50/220 (22.73%)**, down from 23.64%.

Board: PL-side full-network total **1,806.45ms**, down from 2,111.27ms (**-14.4%**; -70.1% cumulative
from this line's original 6,050ms starting point). Single-op board tests all byte-exact: the 4
previously-fallback layers (entry64/66/70/72, 0 mismatches each, combined 446.42ms->143.36ms/-67.9%);
the degenerate-path regression check (entry3 unchanged at 22.67ms, entry60 -- the exact 144KB boundary
layer, first real-chain board test -- byte-exact at 11.89ms). Full-network six-checkpoint correctness
(primary judge per this project's own standing process rule) verified via cosine similarity against
the untouched ONNX float32 reference (`ckpt_ref_*_0000.npy`, dated 2026-08-21): **0.6313/0.1286/0.2227/
0.3491/-0.2459/-0.2811** for stage1/stage2/stage3/stage4/finaldw/se -- EXACT match to the project's own
long-established figures, confirming chunking does not change numeric semantics. Register map is
UNCHANGED from `mac_array_a3_pw_weight_hoist` (`PW_WCHUNK` is entirely internal to the IP body, not a
new register) -- existing ARM-side binaries remain valid, no rebuild needed.

## Prior deployed baseline (superseded 2026-09-03, kept for history)

**`mac_array_a3_pw_weight_hoist` was the deployed baseline from 2026-09-02 to 2026-09-03**, replacing
`mac_array_a3_gmemmeta_elim1` (3,630.74ms/59.25% LUT/WNS+0.272ns, deployed 2026-08-31). This is the
largest single latency win on this project's whole latency-optimization line at the time -- see ZHR-63's
mainline summary and ZHR-92's own round-by-round history for how it was reached, `vivado_impl/
bitstream_archive/mac_array_a3_pw_weight_hoist_2026-09-02/README.txt` for the full technical writeup.

**What changed**: PW's weight read was re-reading the same weight data redundantly from DRAM on every
spatial `(rt,colt)` tile (up to 341.3x redundancy for shallow/wide-spatial layers, 13.22x weighted
average across the 26 real PW layers) -- fixed with a 144KB on-chip weight cache
(`pw_weight_cache`, populated once per layer via a new `PW_WEIGHT_HOIST` loop, before the spatial
sweep begins), sized for the largest of the 22 real PW layers that fit under that cutoff
(`layer_0040_pwconv`). The 4 real layers whose weight exceeds 144KB (`layer_0043/44/47/48_pwconv`,
also the 4 lowest-redundancy real PW layers at 4.0x) fall back to the pre-existing direct-DRAM-read
path unchanged. `PW_CACHED` (cached vs. direct-read) is a runtime bool, not a 2nd template dimension
-- an earlier attempt at templating it alongside the pre-existing `FAST_WRITEOUT` produced 4
independently-synthesized instantiations and physically duplicated `pw_weight_cache` itself; see the
"array parameter read from 2+ instantiations" entry elsewhere in this file for why, and why the
runtime-bool form avoids both the LUT duplication and (checked, not assumed) the `FAST_WRITEOUT`-style
II=2 risk.

Real P&R (route_design alone, no phys_opt needed): **WNS +0.153200ns** (closed, down from +0.272ns but
still positive), **LUT 31,953/53,200 (60.06%)**, up +1.37% from 59.25%, **BRAM 74/140 tiles (52.86%)**,
up from 24.29% -- isolated csynth had projected 70% here, real P&R came in meaningfully better (the
first instance on this project's whole isolated-vs-real line where isolated was more pessimistic than
reality, not more optimistic -- don't assume a consistent error direction). **DSP 52/220 (23.64%)**,
up from 21.82%.

Board: PL-side full-network total **2,111.27ms**, down from 3,630.74ms (**-41.9%**). Two single-op
board tests byte-exact (entry3, the cached path, 0/196,608 mismatches, 50.70ms->22.66ms; entry64, the
`layer_0043_pwconv` fallback-path layer, 0/73,728 mismatches, no regression vs. the old direct-read
path's own 120.68ms). Full-network six-checkpoint correctness verified via cosine similarity against
the untouched ONNX float32 reference (`ckpt_ref_*_0000.npy`, dated 2026-08-21) matching the
project's own long-established figures exactly: 0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811 for
stage1/stage2/stage3/stage4/finaldw/se -- **NOT verified via byte-exact-vs-`ckpt_hw_*`, deliberately**;
see the "Known open issues" entry on why that specific check is currently untrustworthy for ANY
build, not just this one, and is not cited here as a correctness claim. Register map is UNCHANGED
from `mac_array_a3_gmemmeta_elim1` (`PW_CACHED` is entirely internal to the IP body, not a new
register) -- existing ARM-side binaries built for that baseline remain valid, no rebuild needed.

## Prior-prior deployed baseline (superseded 2026-09-02, kept for history)

**`mac_array_a3_gmemmeta_elim1` was the deployed baseline from 2026-08-31 to 2026-09-02**, replacing
`mac_array_a3_dwraster_step2`
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
**CORRECTION, 2026-09-02 (ZHR-92): the "6/6 checkpoints byte-exact vs csim" claim above cannot have
been freshly re-verified as stated.** This round's own interface change (`mac_array_top`'s signature)
is what broke `mac_array_ckpt_dump.cpp`'s compile going forward -- and `tools/
compare_board_full_network_ckpts.py`'s own `ckpt_hw_*` reference was ALREADY stale by this date
(last legitimately generated ~2026-08-23, before this round's own gmem_meta elimination AND before
the 2026-08-28 DW raster integration before it -- see the "Known open issues" entry for the full
timeline). This claim was either citing that already-stale reference without regenerating it, or not
independently re-run at all. **Does not invalidate this round's real, separately-confirmed results**
(the P&R numbers, the +0.61%-flat timing finding, and the single-op byte-exact checks used a
different, unaffected reference) -- only the specific "6/6 checkpoints byte-exact vs csim" sentence.
The board's own correctness at this point in time is NOT disproven by this correction, just no longer
evidenced by this specific claim; nobody has gone back to re-verify gmemmeta_elim1 specifically
against the ONNX reference the way the 2026-09-02 round did for its own successor.

Practical consequence: the new build has ~18 percentage points more LUT headroom than any prior
config on this line. Lines previously closed out for lack of resource/timing margin (DSP packing,
MAC_PD expansion) may be worth revisiting against this new headroom -- flagged, not re-opened as of
this entry. Register map changed completely (every `s_axi_control` offset shifted -- see the archive
README's table); any ARM-side binary built before 2026-08-31 will silently write to wrong addresses
against this bitstream. Always rebuild the driver/test harnesses from current source before using
them against this build.

## Stale-artifact list (started 2026-09-12 -- check here BEFORE reusing any script/data file below)

This project has now hit "a persisted artifact or script looks valid but was generated under a
since-superseded configuration" enough times (the individual entries are scattered through the
working-method section above) that the known-bad items get one consolidated list. Anything here is
NOT to be picked up and used as-is; either regenerate it against the current source or use the
listed replacement. Add to this list whenever a new instance is found; remove an item only when it
has actually been fixed and re-verified, not when a comment says it was.

- **19 `tools/build_*.py` bundle generators still at 27-field descriptors** (`assert len(fields) ==
  27`; `MacLayerDesc` has been 28 fields since 2026-08-31, `use_wide_path` appended). The single-op
  driver rejects a 27-field `desc.bin` outright ("bundle needs regenerating against the current
  28-field layout"). Exact list as of 2026-09-12: `build_dw_ot_probe_large/small.py`,
  `build_dw_tilecount_probe_64.py`, `build_glue_decomp_test.py`, `build_glue_isolation_test.py`,
  `build_nsteps_probe_8.py`, `build_patchhoist_probe_{cin32,cin64,nod_baseline,nod_cin96,
  nod_cout96}.py`, `build_single_op_test_entry{7,9,75_gap,77_relu,79_sigmoid,80_scale}.py`,
  `build_tilecount_probe_{32,64}.py`. The 13 that ARE at 28 fields: `build_full_network_test.py`,
  `build_pw_{axi_txn,burstsize,scaling}_probes.py`, `build_single_op_test_entry{0_gelu,10_add,3,
  5_dw,60,64,66,70,72}.py`.
- **`build_single_op_test_entry{75_gap,77_relu,79_sigmoid,80_scale}.py` specifically**: stale on TWO
  counts -- 27 fields (above) AND their `ref_out.bin` is copied from `accuracy_test_imgs_256/
  entry_NN.bin`, the csim dumps regenerated 2026-09-02 by the same `mac_array_ckpt_dump.cpp` run
  whose `ckpt_hw_*` output is known-wrong (see the open `ckpt_hw_*` issue below). **Replacement**:
  `tools/gen_scalar_ops_csim_bundle.py` (real `desc_all.bin` descriptors, independent Python
  reference) -> `csim_scalar_ops/` for csim (`scalar_ops_real_desc_tb.cpp`) and `board_test_scalar_
  ops/` for the board; both used for the 2026-09-12 promotion.
- **`accuracy_test_imgs_256/ckpt_hw_*_0000.bin`** (csim fixed-point checkpoint reference) and
  **`entry_00..16.bin`, `entry_63/64.bin`**: regenerated 2026-09-02 by a tool whose orchestration
  is known-wrong relative to real board output (two DW implementations gave the same wrong answer),
  root cause never found. `tools/compare_board_full_network_ckpts.py`'s `csim_mism`/`csim_cos`
  columns come from these and are NOT a correctness signal (they show the identical 128423/14208/
  6808/1092/1618/1670 mismatch counts for every build since). **Use the `onnx_cos` column**
  (`ckpt_ref_*_0000.npy`, Python-generated 2026-08-21, untouched) as the correctness judge.
- **`mac_array_ckpt_dump.cpp` + `run_ckpt_dump.tcl`**: compiles again since 2026-09-02 (call-site
  and linked-implementation fixes) but its output is the wrong reference above -- do not regenerate
  `ckpt_hw_*` from it and treat the result as truth.
- **`mac_array_tb.cpp` (legacy 17-phase suite)**: only valid paired with `mac_array.cpp`; against
  the deployed `mac_array_raster_integrated.cpp` it aborts on the `DW_CIN_MIN_SAFE` assertion at
  Phase0 by design. Not a regression signal for the raster architecture. Current raster suites:
  `dw_raster_layer_tb.cpp` (5), `mac_array_raster_integrated_wiring_tb.cpp` (4),
  `gelu_add_burst_tb.cpp` (8), `scalar_ops_real_desc_tb.cpp` (4) -- `run_csim_sohoist_all.tcl` runs
  the first three in one session, `run_csim_scalar_ops.tcl` the fourth.
- **The "four standard csim suites" (raster 5, wiring 4, gelu_add 8, scalar_ops 4) do NOT touch
  PW at all -- a PW-only change can pass all four while broken.** Caught 2026-09-15 (PW_DEFER_WRESP):
  the only PW_FLAT/FAST_WRITEOUT coverage is `pw_weight_hoist_tb.cpp` (6 real shapes incl. chunked
  layers) + `pw_scaling_probe_tb.cpp` (20 synthetic shapes), and BOTH had silently been on the
  9-arg pre-ELEMWISE_BURST `mac_array_top()` signature since 2026-09-06 -- the same class as the
  three stale call-site files below. Re-paired 2026-09-15; `run_csim_pw_suites.tcl` runs both
  (extra flags via the `CSIM_CFLAGS` env var). **The standard set is SIX suites now: 5/5 + 4/4 +
  8/8 + 4/4 + 6/6 + 20/20; run the PW pair for any change that touches `run_layer` or
  `pw_flat_pipeline_impl`.**
- **`dw_linebuf_real_tile.cpp`, `verify_bundle_entry5_dw.cpp`, `writeout_edge_probe/
  writeout_edge_probe.cpp`**: still on the pre-gmemmeta_elim1 `mac_array_top()` signature
  (`&desc, 1, ...`); do not compile against the current header. Fix the call site before use.
- **`accuracy_test_imgs_256/stem_output_0000.bin`**: was regenerated correctly on 2026-08-26 (has a
  sibling `.meta.json`); listed here only as the precedent -- check the meta file's `output_scale`
  matches `shift_table_meta.json` before trusting it after any calibration change.
- **`mac_array_deschoist.cpp`**: a historical, self-contained copy of the whole top (own static
  `run_gelu`/`run_add`/... with the OLD signatures). Not built by any current tcl; do not edit it
  to "keep in sync" and do not mistake its functions for the deployed ones when grepping.
- **Any `#ifdef`-gated variant's numbers in its own comment** (`DWR_ENABLE_FPG_SPECIALIZATION`,
  `LB_FORCE_DSP`, `PW_FORCE_DSP`, `DWR_INPUT_BURST`, `DW_OUTPUT_BURST`, `PW_ALLOW_UNCACHED_FALLBACK`):
  re-measure before citing -- see the "re-measured, never re-cited" rule above.

## Known open issues as of 2026-08-15

- **OPEN (regression cause) / CLOSED (effect), 2026-09-05/07: GELU (+43.8%) and ADD (+46.9%) real
  board time both REGRESSED at the gmem_meta elimination round (2026-08-31), and were flat at the
  regressed value ever since -- the CAUSE was never root-caused (5/5 candidate hypotheses refuted,
  see below), but the EFFECT is now moot: the 2026-09-07 ELEMWISE_BURST round cut GELU/ADD's own real
  time by 93.0%/90.6% for an unrelated reason (their own per-element AXI cost, not the regression),
  taking both from their regressed values down to 22.93ms/13.06ms -- well BELOW even their own
  pre-regression 3,608.76ms-baseline values (228.43ms/94.19ms). Whatever the regression's real
  mechanism was, it is no longer economically relevant to chase given how small GELU/ADD's own
  absolute contribution to the network now is (see the updated operator decomposition below) --
  recorded as a closed line, not further pursued.** Found while re-decomposing the operator-type
  breakdown
  after this session's MAC_PD work eroded PW's own dominance (PW dropped from 60.81% of the old
  3,608.76ms baseline to 32.90% now -- no longer uniquely the largest operator; DWCONV is now
  narrowly ahead at 35.26%). Full current breakdown (fresh full-network run, all 82 entries parsed by
  op_type): DWCONV 531.51ms (35.26%), PWCONV 495.88ms (32.90%), GELU 328.47ms (21.79%), ADD 138.42ms
  (9.18%), SE-block components (SCALE+GAP+RELU+SIGMOID) combined 12.96ms (0.86%). Compared against
  the historical 3,608.76ms baseline's own breakdown (PW 60.81%/2,194.49ms, DW 28.15%/1,015.87ms,
  GELU 6.33%/228.43ms, ADD 2.61%/94.19ms, SE 0.20%/7.22ms): PW dropped -77.4% and DW dropped -47.7%
  (both expected-direction, PW from this session's own optimization work; DW's drop is NOT explained
  by any DW-specific optimization round -- flagged, not investigated, per explicit instruction to
  prioritize the regression over the improvement) -- but **GELU grew +43.8% (228.43->328.47ms) and
  ADD grew +46.9% (94.19->138.42ms), neither of which has ever been touched by any optimization round
  this whole session.**
  **Bisected via real board tests on 3 archived bitstreams spanning the gap (gmemmeta_elim1,
  pw_wchunk, plus the already-known dwraster_step2 and macpd4 endpoints -- register map has been
  unchanged since gmemmeta_elim1, per each promotion's own README, so the current `mac_array_full_
  network_test`/`board_test_full_network` bundle works unmodified against all of them): GELU/ADD are
  ALREADY at the regressed value (328.3-328.7ms / 138.3-138.5ms) at `mac_array_a3_gmemmeta_elim1`
  (2026-08-31), and have stayed flat to within measurement noise (+-0.2ms) through `pw_wchunk` and
  `mac_array_a3_macpd4` (today) -- confirmed NOT a gradual drift across multiple rounds, a single-step
  regression localized to the SAME round that eliminated `gmem_meta`.** DW's own -47.7% drop
  localizes to the identical round (531.3-531.9ms at gmemmeta_elim1, flat ever since) -- both effects
  trace to the same commit, which at first reading looks like it should be impossible (one change
  shouldn't make one op type faster and another slower) but isn't: gmem_meta elimination replaced a
  whole DMA-based descriptor/`out_written` AXI master with an `s_axilite`-based control-register
  mechanism, a protocol change touching EVERY dispatched entry's own completion-signaling, not just
  PW/DW's data path -- a plausible (not yet confirmed) mechanism is that short ops (GELU/ADD, dominated
  by fixed per-dispatch overhead rather than real compute) are disproportionately sensitive to a
  completion-signaling latency change, while DW separately benefited from reduced AXI arbitration
  contention with the now-removed `gmem_meta` traffic -- two distinct effects from one commit touching
  two distinct mechanisms, not one effect misdiagnosed as two. **Not yet root-caused** -- the next
  step (not done this round, per explicit instruction to localize and report, not optimize) would be
  reading `gmemmeta_elim1`'s actual diff against `dwraster_step2` for the ARM-driver-side and
  HLS-side completion-signaling mechanism specifically, now that the search space is a single,
  bounded commit instead of the whole multi-week session.
  **FOLLOW-UP, 2026-09-05, same day: two of three specific hypotheses read from the diff were ruled
  out by direct inspection, and the third (SmartConnect NUM_SI 4->3) was tested via a real isolation
  experiment and CONCLUSIVELY REFUTED for both effects -- the actual mechanism is still unknown.**
  Order-of-magnitude check first (real measurement, not estimate): a standalone ARM-side timing test
  (cross-compiled, run directly on the board, no accelerator dispatch needed) measured the real cost
  of `mac_write_desc()`'s own 28 AXI-Lite register writes at 5,343.5ns total (190.84ns/write average)
  -- for 27 GELU+ADD dispatches, even the full delta vs. a 2-write old mechanism (~4,982.8ns/dispatch)
  is only ~135us total, **1,068x too small** to explain the observed 144.27ms regression. This
  decisively ruled out the AXI-Lite-write-cost hypothesis before any code reading was needed.
  Reading the diff with three specific candidate mechanisms in hand: (1) `run_gelu`/`run_add`'s own
  descriptor field access (`d.in_off`, `d.out_off`, etc.) showed **no structural change** -- identical
  plain-field-read code regardless of how `desc` arrives at the top level, ruled out by direct
  inspection; (2) `out_written`'s move from a `gmem_meta`-DMA'd DRAM location to a scalar `s_axilite`
  output register (`ap_vld`-handshaked, "value stable at ap_done") is real but very likely NOT the
  bottleneck, since ARM-side completion detection has always polled `ap_done` directly (the
  established `usleep`-based polling loops documented elsewhere in this file), never `out_written`
  itself -- this change doesn't add a new wait, just changes how a value already-available-at-the-same-
  moment gets fetched; (3) `sc_data` SmartConnect's own `NUM_SI` dropped from 4 (`gmem_act`/`gmem_w`/
  `gmem_b`/`gmem_meta`) to 3 (`gmem_meta` gone) -- confirmed via a direct BD-tcl diff between
  `dwraster_step2`'s own build script (`NUM_SI 4`, `gmem_meta` on `S03_AXI`) and the current one
  (`NUM_SI 3`), not inferred -- initially the most promising candidate, since it's a genuine change to
  the shared arbitration topology every real master's traffic passes through, and a topology change
  affecting different access patterns differently could explain the "one op type faster, another
  slower" symmetry no single-direction hypothesis could.
  **Isolation experiment (`DUMMY_FOURTH_MASTER`, diagnostic-only, NOT a deployable candidate):**
  restored `sc_data`'s `NUM_SI` to 4 via a new 4th m_axi port (`gmem_dummy`) that is genuinely LIVE in
  the RTL (gated by `dummy_enable`, a real `s_axilite` runtime bool HLS cannot prove false at compile
  time, so the port cannot be optimized away) but never actually issues a transaction in real dispatch
  (the ARM driver never writes `dummy_enable` true, so it stays at its POR-reset default of 0) --
  isolating "does 4-way arbitration matter" from "does `gmem_meta`'s own specific traffic pattern
  matter". csim clean (4/4, `mac_array_raster_integrated_wiring_tb.cpp`). Real P&R: route_design alone
  gave WNS=-0.133008ns (a real violation, LUT 82.57%/+1.78pp over macpd4's 80.79%) -- recovered to
  WNS=+0.013ns (thin but positive) via a single-threaded `phys_opt_design` pass on the same
  checkpoint, per this project's own established two-phase recipe. Board: single-op byte-exact
  (entry3, 0/196,608 mismatches, 17.30-17.32ms, matching macpd4 exactly -- confirms the new register
  offsets, appended after all existing ones, don't shift anything already deployed). **Full network,
  2 repeats: GELU 328.70/328.83ms, ADD 138.53/138.48ms, DW 531.86/531.97ms, PW 496.37/496.38ms -- ALL
  four essentially IDENTICAL to the macpd4 baseline's own values (328.47/138.42/531.51/495.88),
  reproducible across both repeats.** Restoring 4-way arbitration did NOT bring GELU/ADD back down
  toward their old values (228.43/94.19ms) even partially, and did NOT reverse DW's own drop either --
  **the NUM_SI hypothesis is REFUTED for BOTH effects, not just one.** This is a cleaner, more
  decisive null result than the pre-registered "if only GELU/ADD move and DW doesn't, NUM_SI explains
  only half" contingency anticipated -- neither moved at all, meaning NUM_SI explains none of it.
  Board reverted to `mac_array_a3_macpd4` (byte-exact re-confirmed), golden untouched. **All three
  specific hypotheses read from the diff are now ruled out (field access: no change found; out_written:
  architecturally unlikely to matter; NUM_SI: isolated and refuted by direct experiment) -- the actual
  mechanism behind BOTH the GELU/ADD regression and DW's improvement remains unknown.** The `n_layers`
  loop removal (item 1 of the original three questions, `desc[]` array -> single by-value struct) was
  never isolated on its own and is the one remaining candidate from the original diff-reading pass;
  beyond that, no new hypothesis has been proposed. `DUMMY_FOURTH_MASTER`'s source changes (`mac_array_
  raster_integrated.cpp`/`mac_array.h`/`mac_array_raster_integrated_wiring_tb.cpp`) were an
  unconditional signature change (a new `gmem_dummy`/`dummy_enable` parameter pair, not gated behind
  an `#ifdef`) -- unlike this project's usual probe convention (kept behind an off-by-default macro,
  base signature untouched), a bare `#ifdef` around function PARAMETERS doesn't compose cleanly in
  C++, so once the experiment concluded the source was reverted to the deployed baseline exactly
  (`git checkout`, confirmed zero remaining `gmem_dummy`/`dummy_enable` references) rather than left
  in place -- matching this project's own "attempted, measured, reverted" precedent for one-off
  structural experiments (e.g. option D's desc-local-copy attempt) instead of the "kept behind ifdef"
  precedent that applies to internal-behavior-only probes. The isolated-csynth/P&R/board-test tcl
  scripts for this experiment are kept for the historical record even though the source itself is
  reverted.
  **FIFTH AND FINAL CANDIDATE REFUTED BY MAGNITUDE ALONE, 2026-09-06 (no experiment needed): the
  `n_layers` loop removal / 28-register startup fan-out was ruled out by the same order-of-magnitude
  argument as the AXI-Lite-write check, without spending a board round on it.** 144.27ms / 27
  GELU+ADD dispatches = 5.34ms/dispatch = 534,000 cycles at 100MHz -- no plausible HLS-generated
  register-fan-out sequence (distributing 28 already-parallel `s_axilite` registers to their
  consumers) costs anywhere near that many cycles; even the already-measured real dispatch floor
  (~0.58ms/entry = 58,000 cycles, itself 9x smaller and covering far more than register fan-out
  alone: AP_START handshake, full descriptor validation, etc.) is an order of magnitude too small.
  **All five candidates read from the gmem_meta-elimination diff or otherwise proposed are now
  exhausted (AXI-Lite write cost, `run_gelu`/`run_add` field access, `out_written` mechanism,
  SmartConnect `NUM_SI`, register-fan-out startup cost) -- the actual mechanism behind the GELU/ADD
  regression AND DW's own improvement is recorded as OPEN, not pursued further.** Cheap-argument
  refutation (checking whether a hypothesis is even physically plausible before spending a board
  round) is a real, repeatable technique on this project now -- used decisively twice in a row on
  this same investigation (this entry and the AXI-Lite one above) to close a candidate in minutes
  instead of a full isolation-experiment round.
  **PIVOT (same day, explicit direction change, not a continuation of the regression search):**
  rather than keep searching for the regression's CAUSE, decomposed GELU/ADD's own real cost
  STRUCTURE directly -- these two operators have never been individually profiled in this whole
  project (nine-plus rounds of optimization work, all on PW), and now account for 466.89ms/30.68%
  of the network, comparable to PW (32.90%) and DW (35.26%). Per-entry decomposition (element count
  vs. real ms, all 17 GELU + 10 ADD real dispatches, matched against a naive `elements/cycle @ II=1,
  1-wide` theoretical floor): **GELU's real-to-theoretical ratio is 8.42x-8.87x, essentially FLAT
  across a 32x element-count range (24,576 to 786,432 elements) -- confirmed a genuine PER-ELEMENT
  cost, not fixed dispatch overhead.** ADD's ratio is 15.94x-17.66x, same flat-across-size pattern.
  Checked the actual HLS scheduling diagnostic (not assumed): GELU's own csynth report claims a
  PERFECT achieved II=1; ADD shows the exact `Unable to schedule bus request operation... due to
  limited memory ports` signature this project has hit dozens of times before (reading `in_off`/
  `in2_off` simultaneously from the shared `gmem_act` bundle forces achieved II=2) -- ADD's own
  extra ~2x over GELU (16.57/8.60=1.93x) is consistent with this confirmed II=2, but GELU's own
  8.60x is a REAL-HARDWARE-ONLY gap invisible to HLS's own "perfect II=1" claim -- the same class of
  isolated-csynth-vs-real divergence this project has documented many times, here manifesting as a
  scheduling-feasibility metric (II) that says nothing about whether a plain, un-bursted, one-
  element-at-a-time AXI transaction's real DRAM round-trip latency gets hidden across iterations.
  Both `run_gelu` and `run_add` used plain `act_t[]` pointer accesses before this round -- neither
  had ever used `hls::burst_maxi`, unlike PW's `ROW_READ`/`WRITEOUT`, which fixed the identical
  mechanism for PW's own AXI paths years (in this project's own timeline) earlier. Confirmed via
  source read: both loops are genuinely 1-wide (no `UNROLL` anywhere), unlike PW's 64-wide
  (`MAC_PD*MAC_PR*MAC_PC`) or DW's own parallel line-buffer structure -- a completely untouched
  optimization axis.
  **IMPLEMENTED, ELEMWISE_BURST, csim clean (per explicit instruction to stop there this round):**
  rewrote `run_gelu`/`run_add` to use two new `hls::burst_maxi<act_t>` ports (`elemwise_in_burst`/
  `elemwise_out_burst`), sharing the EXISTING `gmem_act` bundle (no new AXI master, no BD change --
  mirrors `out_burst`/`in_burst`'s own established "share a bundle, get a separate control register"
  precedent). No documented exact `hls::burst_maxi` single-request length ceiling was found locally
  (the shipped csim model only asserts `len>0`) -- sidestepped by design rather than gambling on an
  unverified large single request: both loops chunk at a new `ELEMWISE_CHUNK=4096`-element compile-
  time bound, in a plain sequential (non-unrolled) outer loop with a genuine runtime trip count (the
  real largest GELU tensor, 786,432 elements, needs 192 chunks) -- no compile-time-bound requirement
  applies to that outer loop, only the inner per-chunk `PIPELINE`d loop needs one, matching this
  project's own established "runtime value gating a compile-time-bounded loop" pattern exactly.
  `run_add`'s own two-source case reads `in_off`'s chunk into a small on-chip buffer first, then
  reads `in2_off`'s matching chunk and sums on the fly while writing -- two SEQUENTIAL passes over
  the SAME burst port, not two simultaneous reads, deliberately avoiding the exact mechanism that
  caused the old version's confirmed II=2 violation (whether this incidentally restores II=1 is a
  csynth-stage question, not assumed here). No existing testbench in this codebase exercises GELU/
  ADD against the current raster architecture (`mac_array_tb.cpp` is the already-documented stale-
  paired legacy suite; `mac_array_raster_integrated_wiring_tb.cpp` only covers DW) -- wrote a new,
  dedicated, self-contained testbench (`gelu_add_burst_tb.cpp`, reference values computed by
  replicating `quantized_sigmoid`/`clip_shift`'s own exact arithmetic in-tb, no external golden file
  needed) covering small/exact-chunk-boundary/multi-chunk-even/multi-chunk-uneven-last-chunk/real-
  network-scale (786,432 elements, layer 0's own GELU shape) cases. **csim: 8/8 PASS, 0 mismatches**,
  including the uneven-last-chunk case (the real risk point for the new chunking loop) and the full
  network-scale case. The pre-existing DW wiring testbench (`mac_array_raster_integrated_wiring_tb.
  cpp`, whose own call site needed updating for the 2 new parameters) still passes 4/4, confirming
  the shared `mac_array_top` signature change didn't disturb DW. **Pre-registered expectation for
  the next steps (csynth/P&R/board, not yet run): if burst access drops the per-element cost from
  ~8.6x toward ~2x (one real read + one real write, a reasonable bursted floor), GELU -> ~76ms, ADD
  -> ~33-66ms depending on whether II=2 also resolves, combined 466.89ms -> 110-142ms, full network
  1,522.39ms -> a projected 1,200-1,350ms (-11% to -21%) -- flagged explicitly as likely optimistic,
  per this project's own repeated finding that real board gains usually land below naive projections.**
  **REVISED, same round, before csynth: the 8-bit `hls::burst_maxi<act_t>` design crashed csynth's own
  codegen.** `Call parameter type does not match function signature!` / `_ssdm_op_Write.m_axi.p1i32`
  expecting a 32-bit write / `Broken module found, compilation aborted!` -- a 2nd confirmed instance
  of this project's internal-compiler-crash class (after the 2026-08-24 burst_maxi same-bundle probe),
  a DIFFERENT trigger this time: mixing an 8-bit burst_maxi port onto the SAME bundle as the EXISTING
  32-bit `out_burst`/`in_burst` ports.
  **CORRECTED, 2026-09-08 (DWR_INPUT_BURST/DW output-writeout round): the scope stated here was
  imprecise and would have misfired on a later round if taken literally.** This is NOT "plain pointer
  + burst_maxi sharing a bundle is safe only because the pointer happens to match the bundle's 32-bit
  width" -- checked directly: `in_base` is `act_t*` (8-bit), not 32-bit, and it has coexisted with
  32-bit `in_burst`/`out_burst`/`elemwise_*_burst`/`dw_in_burst` on `bundle=gmem_act` throughout this
  entire session with zero crashes. **The real, narrower trigger is specifically two DIFFERENT-WIDTH
  `hls::burst_maxi` PORTS sharing one bundle -- a plain pointer of ANY width coexisting with a
  `burst_maxi` of ANY width on the same bundle has never crashed, at any point in this project's
  history.** Before citing this crash as a reason to avoid a new plain-pointer-plus-burst_maxi
  combination, check which of the two actual triggers applies -- conflating them would incorrectly
  block combinations (like this session's own default architecture) that are already proven safe.
  Original entry's own fix was correct regardless (making the new port 32-bit rather than 8-bit) --
  only the STATED reason for why it was needed was imprecise. Fixed by making
  `elemwise_in_burst`/`elemwise_out_burst` `ap_uint<32>`-typed (matching the bundle) with 4-byte pack/
  unpack inside `run_gelu`/`run_add` (4 lanes unrolled per word -- a bonus 4x-per-cycle parallelism
  neither the old design nor the pre-registration accounted for) -- verified SAFE first, per this
  project's own "verify contiguity/alignment before batching" rule: real descriptors (all 27 GELU/ADD
  entries, `tools/layer_hw_sequence_256.json`) confirmed `in_off`/`in2_off`/`out_off` AND `total`
  (=cin*h*w) are ALL exactly mod4==0 (real network channel counts and spatial dims are always
  multiples of 4) -- zero tail-byte handling needed anywhere. One csim regression caught by the fix
  itself: the testbench's own "uneven chunk boundary" case (`ELEMWISE_CHUNK*2+137`) turned out to
  violate the SAME mod4 invariant the fix relies on (137 mod4=1) -- an unrealistic test value, not a
  real implementation bug (`total` is never non-mod4 on any real dispatch) -- fixed to `+136` (mod4==0)
  to correctly exercise an uneven CHUNK COUNT without violating the real-network alignment invariant.
  csim 8/8 clean again after both fixes; DW wiring testbench 4/4 clean.
  **Isolated csynth confirmed all 4 pre-registered judgment items**: burst inference on all 5 access
  points (`ManualBurstInstancePassed`, `Length="variable"`, `Width=32`, `gmem_act`); resource delta
  small (LUT +4.6%/+3,268, DSP +12%/+6, BRAM_18K +0.8%/+2 -- `buf_a`'s size checks out exactly:
  `ap_uint<32>[1024]`=32,768 bits needs precisely 2 RAMB18E1 tiles, matching the observed delta
  exactly); **ADD's achieved II resolved from 2 back to 1** -- a genuine bonus, not in the original
  pre-registration, confirmed via the actual csynth diagnostic (`ADD_READA`/`ADD_COMPUTE` both
  `Target II=1, Final II=1`, no violation) -- the sequential two-pass read design (buffer `in_off`'s
  chunk fully before reading `in2_off`) genuinely eliminated the old simultaneous-read port contention
  that forced II=2 before; GELU's own II=1 unchanged.
  **Real P&R: route_design alone gave WNS=-0.166790ns (violated).** Checked the critical path before
  reaching for any lever (per this project's own stop-loss discipline): all top-10 paths were IDENTICAL
  in shape to macpd4's own pre-existing critical path -- `run_layer`'s FSM state -> `mul_32s_32s_32_2_1`
  (the shared address-arithmetic multiplier), 4 logic levels, 60.7%/39.3% logic/route split -- **the
  new GELU/ADD burst logic appeared nowhere in the top-10.** This confirmed the violation was resource-
  pressure-induced placement degradation on the SAME pre-existing mechanism (real utilization: LUT
  83.22%/+2.43pp, BRAM 76.43%/+0.72pp, DSP 26.82%/+4.09pp over macpd4), not a new bottleneck from the
  burst rewrite itself -- the isolated LUT extrapolation (84.5%) again didn't hold in either direction
  cleanly (real came in lower), the 8th "isolated vs real" data point on this project's own running
  tally. A single `phys_opt_design` pass (NOT pblock tuning, NOT directive rotation -- neither on the
  hard-stop list, and `phys_opt_design` had already recovered a comparable thin negative margin once
  this session, dummy4th's -0.133->+0.013ns) recovered **WNS=+0.017ns** -- one pass only, per the
  project's own established two-phase recipe and this round's own explicit "one pass, don't iterate"
  discipline.
  **BOARD DEPLOYMENT HIT A REAL HANG, then a real wrong-output case -- root-caused to TWO separate,
  pre-existing bugs, NEITHER of which was in the ELEMWISE_BURST HLS logic itself.** First: dispatching
  the largest real GELU entry (786,432 elements, layer 0) via `board_test_entry0_gelu` (built but
  NEVER previously pushed/run -- confirmed via board directory listing) hung the board completely
  (unresponsive to SSH AND ping -- more severe than either of this project's two prior documented
  hangs, both of which at least answered ping). User power-cycled; board recovery followed this
  project's own established checklist (fresh uptime confirmed at "0:00", golden image md5 re-verified
  unchanged, `mac_array_a3_macpd4`'s own already-proven `.bit` used as PL-reconfiguration proof-of-life
  per the established substitute-golden precedent, single-op sanity before trusting anything larger).
  **Root cause #1, found while investigating: `tools/build_single_op_test_entry0_gelu.py` and
  `tools/build_single_op_test_entry10_add.py` were BOTH stale at 27-field descriptors** (`MacLayerDesc`
  grew to 28 fields on 2026-08-31 when `use_wide_path` was appended -- these two scripts were never
  updated, and -- confirmed via the board's own directory listing -- their bundles had NEVER actually
  been built or run before this round, unlike `build_single_op_test_entry3.py`'s own 28-field, daily-
  used, known-working list). Fixed both scripts (append `use_wide_path=0`, 27->28 fields/bytes) --
  this exact failure class (a persisted artifact silently generated under a since-superseded
  configuration) is already extensively documented elsewhere in this file; this is simply a new
  instance of it, on test-bundle generator scripts this time rather than calibration data.
  **Root cause #2, the actual proximate cause of both the hang and the wrong output: `elemwise_in_
  burst`/`elemwise_out_burst` (this round's own new burst ports) were NEVER wired into the ARM driver
  -- their own base-address AXI-Lite registers (found via the exported IP's real `xmac_array_top_hw.h`:
  0xe8/0xec and 0xf4/0xf8) were left completely unprogrammed on every real dispatch.** This is the
  EXACT risk this round's own header comment on these ports had already flagged in advance ("own
  control register, not yet wired into mac_array_driver.c... a P&R-stage TODO, tracked, not silently
  deferred") -- csim cannot catch this at all (no register-address concept), and it manifested exactly
  as this project's own established precedent for this failure class predicts: `elemwise_in_burst`
  reading from an unprogrammed/garbage address caused a genuine AXI bus hang (the large-GELU case);
  `elemwise_out_burst` writing to an unprogrammed address caused output that was 100% byte-identical
  to the pre-dispatch poison pattern despite `ap_done`/`out_written=1` firing normally (the moderate-
  ADD case, no hang, just silently wrong -- the exact "IP completes, output silently not written"
  signature this project's own `mac_array_tb.cpp` header comment already names as a known defect
  class). Fixed by adding `W64(MAC_ELEMWISE_IN_BURST_LO/HI, ...)` and `W64(MAC_ELEMWISE_OUT_BURST_LO/
  HI, ...)` calls to ALL THREE real ARM-side call sites (`mac_array_single_op_test.c`, `_add.c`,
  `mac_array_full_network_test.c` -- grepped for every real caller, matching this project's own
  established discipline for interface changes), pointed at the SAME physical address as `in_base`/
  `out_base` respectively, exactly mirroring `in_burst`/`out_burst`'s own already-fixed precedent.
  **Real board result after BOTH fixes, decisively better than the pre-registered range at every
  level:** entry0_gelu (786,432 elements, the exact shape that hung) -- byte-exact, 0/786,432
  mismatches, 3.35ms (vs. the old mechanism's 66.82ms for this shape, **-95.0%**, ~19.9x); entry10_add
  (196,608 elements) -- byte-exact, 0/196,608 mismatches, 2.18ms (vs. 31.33ms old, **-93.0%**, ~14.4x).
  Full network (82/82 written, 6/6 checkpoints): **GELU 328.47ms->22.93ms (-93.0%), ADD 138.42ms->
  13.06ms (-90.6%), PW 495.88ms->496.08ms (unchanged, no regression), DW 531.51ms->531.20ms (unchanged,
  no regression), full network 1,522.39ms->1,099.77ms (-27.8%)** -- well below the pre-registered
  1,150-1,320ms range's own optimistic end. GELU/ADD no longer appear anywhere in the top-10 most
  expensive real entries (previously dominant). ONNX cosine EXACT match at every checkpoint
  (0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811) -- numeric semantics fully preserved. This closes out
  a real, substantial fraction of the "91.4% real-hardware-only, unexplained" question this whole
  operator-decomposition line opened with -- not the whole 91.4% (PW/DW's own real-hardware-only share
  is untouched by this round), but GELU/ADD's own entire contribution to it.
  **Not yet resolved as of this entry: WNS=+0.017ns falls in this project's own newly-established
  middle band (+0.01 to +0.1ns -- board-testable, real, but NOT promoted to deployed baseline unless
  the benefit is exceptionally large) -- whether -27.8% full-network qualifies as "exceptionally
  large" enough to promote despite the thin margin is an explicit judgment call left to the user, not
  decided in this entry.** A pre-registered fallback lever exists if a more robust margin is wanted:
  halving `ELEMWISE_CHUNK_WORDS` (1024->512, buf_a 4KB->2KB) would shrink BRAM/related logic at the
  cost of doubling transaction count per tensor -- flagged as plausible-but-untested (the 8.60x/16.57x
  per-element cost was dominated by per-element AXI overhead, not chunk-transaction count, so doubling
  transaction count from ~2/tensor-region to ~4 is expected to still land far below the old un-bursted
  cost), not attempted this round.
  **RESOLVED, 2026-09-07: promoted to deployed baseline (`mac_array_a3_elemwise_burst`) -- the -27.8%
  full-network result was judged the exception the "+0.01 to +0.1ns / not auto-promoted unless the
  benefit is exceptionally large" rule anticipated: the SECOND-largest single-round win on this whole
  line (behind only PW weight-residency's own -41.9%).** See the deployed-baseline section above for
  the full characterization, including the explicit "this margin has zero headroom, came from
  `phys_opt_design` not `route_design` alone" annotation this promotion carries.
  **Operator decomposition REDONE after this promotion (same full-network run, no new board time
  needed)**: DWCONV 531.20ms (49.36%), PWCONV 496.08ms (46.09%), GELU 22.93ms (2.13%), ADD 13.06ms
  (1.21%), SE-block components combined 12.95ms (1.20%). **GELU+ADD combined dropped from 30.68% to
  3.34%** -- matching the pre-registered ~3.3% estimate almost exactly. DW and PW are now essentially
  TIED (49.36% vs. 46.09%, 95.45% combined) -- neither is uniquely dominant the way PW was at the
  start of this whole latency-optimization line (60.81% of the original 3,608.76ms baseline). **This
  sets the next real target: whichever of PW or DW is chosen, GELU/ADD/SE are no longer worth
  attacking (combined 4.54% of the network) -- the "91.4% real-hardware-only, unexplained" question
  from the operator-decomposition round's own opening is now entirely a PW/DW question, not a
  GELU/ADD one.**
  **Global tally, this whole latency-optimization line**: 6,050ms (original) -> 1,099.77ms (current),
  cumulative **-81.8%**.
  **`phys_opt_design`'s own applicability boundary recorded as a standing rule** (see the deployed-
  baseline section above for the full 3-data-point derivation): roughly **within -0.2ns** of a
  route_design-alone violation, a single pass reliably recovers positive WNS; beyond that (the
  -2.264ns PW_WCHUNK case), it needs an actual source-level fix instead. Usable directly as a
  decision rule for future rounds without re-deriving it.
- **CLOSED, 2026-09-07 (ZHR-92, DWR_INPUT_BURST): attempted the same burst_maxi fix that gave
  GELU/ADD their real -93%/-91% win, on DW's own per-pixel `in_base[]` read -- real board result was
  a REGRESSION (DW +8.1%, full network +3.5%), not an improvement, and the round's own root-cause
  chase found WHY.** Motivated by a decomposition mirroring GELU/ADD's own: DW's real-to-naive-floor
  ratio came back 18.80x (7.48x once corrected for padding and `dwr_consume`'s own header-commented
  "achieved II=2"), matching GELU's 8.60x closely enough to predict the same fix would work.
  Implementation: `dwr_prefetch_channel` bursts a whole channel's real (unpadded) plane via
  `hls::burst_maxi<ap_uint<32>>` (`dw_in_burst`, new port, chunked at `ELEMWISE_CHUNK_WORDS`,
  matching ELEMWISE_BURST's own chunk size) into an on-chip buffer BEFORE `dwr_produce`'s own
  `PIPELINE II=1` COL loop starts, which then reads the buffer instead of `in_base[]` directly.
  Alignment verified structurally before implementing (not assumed): `in_off`, `in_ch_stride`
  (=h_in*w_in), and every real channel's start address are mod4==0 for all 25 real DW layers,
  guaranteed since h_in/w_in are always powers of two >=8 -- zero tail-byte handling needed.
  csim clean (`dw_raster_layer_tb.cpp` 5/5, wiring tb 4/4, `gelu_add_burst_tb.cpp` 8/8 regression).
  Isolated csynth: burst inference confirmed (`ManualBurstInstancePassed`), `dwr_consume`'s own
  achieved II confirmed **8, not the "2" this file's own header comment claims** -- checked against
  the elemwise_burst-only baseline's own report and confirmed this discrepancy predates this round,
  not introduced by it (this is the same discrepancy an earlier round already flagged as unresolved --
  see the `PW_FORCE_DSP`/`LB_FORCE_DSP` entry above). Isolated LUT delta +804/+1.09% over the
  elemwise_burst-only baseline -- right at this round's own pre-registered ">1% likely won't pass"
  threshold.
  **Real P&R: route_design alone WNS=-0.103291ns (within the established ~-0.2ns `phys_opt_design`
  boundary), one pass recovered WNS=+0.004ns.** This falls in the SAME non-deliverable band as an
  earlier-session +0.0036ns precedent (<+0.01ns) -- NOT promoted, NOT deployed as baseline, but board-
  tested anyway per explicit instruction (measurement, not deployment) since "board test" and
  "promote" are different decisions and the earlier +0.0036ns case was also board-tested for its own
  real number. Real utilization: LUT 44,172/53,200=83.03% (essentially flat vs. the elemwise_burst
  baseline's 83.22% -- the isolated +1.09% did not survive real P&R, a 9th isolated-vs-real
  divergence instance, this time favorable), BRAM 115/140=82.14% (+8 tiles, half the isolated +16
  projection), DSP 62/220=28.18% (+3, matching isolated exactly). Critical path confirmed unchanged
  before AND after `phys_opt_design` (`mul_32s_32s_32_2_1`/`pw_flat_pipeline_impl_true`, the same
  pre-existing mechanism every other thin-margin build on this line has hit) -- not new DW code.
  **Board protocol**: driver register (`dw_in_burst`, offset 0x100/0x104, read from the fresh export's
  own `xmac_array_top_hw.h`) was wired into all three real ARM call sites BEFORE the first board
  attempt this time, not after a hang -- the first time this project's own repeated "shared bundle !=
  shared register" trap (4th confirmed instance) was closed pre-emptively instead of discovered via a
  real failure. Single-op entry5_dw byte-exact first, then full network run TWICE with the SAME
  bitstream, comparing all 6 checkpoints + entry81 byte-for-byte -- **all 7 files MD5-identical across
  both runs** (times 1138.83ms/1138.90ms, within noise), confirming this is a real, deterministic
  result and not a marginal-timing artifact (same two-run bit-identity technique this project has used
  twice before for timing-violating builds, applied here to a technically-MET-but-razor-thin one).
  **Real result: DW 531.20ms -> 574.22ms (+8.1%, WORSE), full network 1,099.77ms -> ~1,138.87ms
  (+3.5%, WORSE).** PW/GELU/ADD unaffected (within noise); all 6 ONNX cosine checkpoints exact match
  (0.6313/0.1286/0.2227/0.3491/-0.2459/-0.2811), correctness fully preserved. Board reverted to
  `mac_array_a3_elemwise_burst` (re-verified byte-exact, 38.75ms on entry5_dw), golden untouched.
  **Root cause, found by redoing the ORIGINAL decomposition's own floor calculation with a number
  this SAME round's own csynth had already measured but hadn't been used to correct it**: recomputing
  the "adjusted floor" with the REAL achieved II=8 (not the stale "2" the file's own comment claimed)
  gives the baseline's own real ratio as only 1.87x, not 7.48x -- `dwr_consume`'s own reduction/
  writeout pipeline, not the input read, was always DW's real bottleneck, and it was already close to
  its own floor before this round touched anything. This build's ratio came back 2.02x, slightly
  worse than baseline's 1.87x. **SECOND CONFIRMED INSTANCE of this file's own "pulled out of
  pipeline overlap, cost becomes additive" mechanism** (1st: output-write batching, `PW_WRITEOUT_FLUSH`,
  see the "row-batched WRITEOUT... reverted" entry above): pulling an access out of an already-
  pipelined, latency-hidden context into its own serial pre-stage (`dwr_prefetch_channel` runs to
  completion before `dwr_produce`/`dwr_consume`'s own `DATAFLOW` region starts, with no overlap) makes
  its real per-transaction cost fully additive instead of hidden -- compounded here by optimizing the
  wrong side of the pipeline entirely, since `dwr_consume`'s own II=8 (not the input read) was always
  the binding constraint. Two independent instances now, different files, different mechanisms being
  batched (output writes vs. input reads) -- treat this as a general property of this codebase's
  DATAFLOW-region pipelining, not a one-off, before proposing any future "move an access into its own
  stage" restructuring.
  **STANDING RULE ADDED, 2026-09-08: any performance number taken from a code COMMENT (an achieved
  II, a latency, a cycle count) must be verified against the actual csynth report BEFORE being used
  as the basis for a new experiment's design -- not just before being cited as a historical fact
  (this file's own pre-existing "a comment asserting an invariant needs independent verification"
  rule already covered the latter; this extends it to the former, a costlier failure mode).** This
  round's entire premise (DW's "7.48x adjusted floor," the whole reason input-side burst looked worth
  a full implementation+P&R+board round) was built on `dwr_consume`'s own header comment claiming
  "achieved II=2" -- a claim, never re-checked against the csynth report before the decomposition
  used it as an input. The real number (II=8, confirmed via this SAME round's own csynth, checked for
  an unrelated reason -- whether II had regressed) was sitting in a report file the whole time and
  would have shown the true floor was already 1.87x, not 7.48x, before a single line of the
  implementation was written. A comment doesn't participate in any automatic check (compiler,
  csynth, csim) -- it can silently drift out of sync with the code it describes the moment the code
  changes, and nothing forces a re-sync. **Before designing an experiment whose target/expected-
  benefit calculation depends on a specific II/latency/cycle-count number, grep the actual csynth
  XML/report for that number (`<PipelineII>`, the loop table's own "achieved" column) rather than
  trusting a comment's claim of it -- this is now the second time in this project a stale performance
  claim was discovered only AFTER spending a full round on it (the first was `DWR_ENABLE_FPG_
  SPECIALIZATION`'s own "93.6% DSP" claim, caught before implementation there; this time the flawed
  premise wasn't caught until the round was already complete).**
  **Disposition**: kept in source as `DWR_INPUT_BURST` (OFF by default), matching this
  file's own established convention for real-but-rejected mechanisms (`DWR_ENABLE_FPG_SPECIALIZATION`,
  `LB_FORCE_DSP`, `DWR_HOIST_BASE_ADDR`) rather than deleted -- default path restored to the original
  direct `in_base[]` read (re-verified csim-clean: 5/5, 4/4, 8/8, matching pre-round behavior exactly).
  **Practical consequence for any future DW timing work on this line: target `dwr_consume`'s own
  achieved II=8, not the input read.**
  **RE-EVALUATED AFTER II 8->2, 2026-09-13 (analysis only, no build) -- DWR_INPUT_BURST stays
  CLOSED; the arithmetic that was wrong last time was redone with the right II and the right
  base.** Question: with `dwr_consume` now at achieved II=2 (`mac_array_a3_dwob`, DW 329.0ms), is
  `dwr_produce` (II=1, plain per-pixel `in_base[]` AXI reads) still hidden behind it, and if not,
  by how much? Method: per-layer real board times from the same full-network run, all 25 real DW
  layers, against the input-pixel count `cin*h_pad*w_pad` (= both pipelines' trip count) and the
  output count, on BOTH the II=8 (`sohoist`) and II=2 (`dwob`) baselines.
  - Fit (R^2 0.992): DW = **4.72 cycles/input-pixel + 7.45 cycles/output + 160/channel** = 168 +
    156 + 7ms (II=8: 7.85 / 11.47 / 273 = 279 + 240 + 12ms). The two terms are collinear (outputs
    ~ pixels/S^2), so the split is indicative, not exact.
  - **The decisive per-layer check: the 6-cycle II reduction materialised as -5.2 to -6.9
    cycles/input-pixel on 23 of the 25 layers -- essentially 1:1.** If produce were the binding
    side at X cycles/pixel, consume's II drop would have saturated at X; it did not. Consume is
    still the bottleneck; produce is still hidden on those layers.
  - The exceptions: **layer 1** (48ch, 128x128, k3, s2 -- 811k pixels, 23% of all DW input pixels):
    only -3.99, i.e. ~2.0 cycles/pixel of the II saving did NOT materialise = **~16ms** -- the one
    real "produce exposed" signature in the network (fast consume: stride 2, few outputs). Layers
    63/69 (384ch, 8x8, k7): -4.3, but at N=196 per channel that shortfall is the per-channel fixed
    cost (weight/bias loads, dataflow start), not produce. Everything else within ~1 cycle of -6.
  - **Exposed produce, best estimate ~20ms; hard upper bound 57ms** (the stride-2 layers' ENTIRE
    excess over the consume-II floor, 83.0 - 25.6ms, attributed to produce -- which it isn't, output
    writes are in there too). Against the pre-registered thresholds (>100ms reopen / <30ms don't):
    **don't.** And the additive cost of the prefetch is a REAL MEASUREMENT, not an estimate: the
    2026-09-07 board run at II=8 (produce fully hidden) came back +43ms -- that IS the prefetch's
    serial cost (~1.2 cycles/pixel over 3.55M pixels). Net if reopened: ~20 - 43 = about -23ms,
    a regression again, at best break-even at the 57ms bound.
  - Where DW's time actually is now (from the same fit and the stride-1 layers' ~10.8 cycles/
    pixel vs the II=2 floor of 2): the OUTPUT side -- ~7.5 cycles per output, ~156ms, i.e. the
    packed write's `write_request`/`write`/`write_response` latency serialised inside the II=2
    `CROW_CCOL` pipeline (the `200-880` carried dependence), not the input read and not the II
    itself (the II=2 floor over all DW pixels is only 71ms of the 329). If DW is attacked again,
    that is the lever; DWR_INPUT_BURST targets the smaller, mostly-hidden component and makes it
    additive.
  - Code check: the dormant `DWR_INPUT_BURST` code still compiles and passes `dw_raster_layer_tb`
    5/5 with `-DDWR_INPUT_BURST` on the current source (packed writeout + SHARED_MUL_ARMS) --
    `dwr_produce_burst` feeds the same `taps` stream, and `dwr_consume`'s interface change was on
    the output side only. Compatible, just not worth opening (`run_csim_dwrin_check.tcl`).
  **Lesson, same family as the 2026-09-08 one that started this line: the input-side verdict
  depends on the consume II, so it has to be re-derived every time the consume II changes -- and
  it was, this time BEFORE building anything. The 1:1 "did the II saving materialise per layer"
  check is a cheap, board-data-only way to tell which side of a producer/consumer pair is binding;
  it needs two real runs at two different IIs, which this line now has.**
  **DW OUTPUT SIDE COSTED, 2026-09-14 (analysis only, no build): a second physical write port is
  NOT worth it, the 8-byte lane merge is infeasible, and the premise "II 2->1 halves the 156ms" is
  wrong -- the 156ms is per-write ROUND-TRIP STALL, not II, and the lever is handshake COUNT.**
  1. Second write port: `bundle=gmem_act` already carries 8 ports (`in_base`, `out_base`,
     `in_base_wide`, `out_burst`, `in_burst`, `elemwise_in/out_burst`, `dw_in_burst`) -- all on ONE
     adapter with ONE AXI write channel; a 9th port on the same bundle gives nothing (the `200-880`
     is "two bus writes on port gmem_act per cycle", a per-bundle limit). A real second write path
     means a second BUNDLE = a new AXI master: BD change (`sc_data` NUM_SI 3->4), a new adapter
     (~+950 LUT, from `dummy4th`'s measured +1.78pp), a new driver register per port on it (the
     shared-bundle trap), and -- the two data points this line has for "add a master" -- **~-0.4 to
     -0.5ns of route-only WNS** (`gmem_act_wide`'s 5th master ~0.4ns; `dummy4th` on macpd4: +0.339
     -> -0.133). From +0.172 that lands at about -0.25..-0.33, at/over the phys_opt band edge.
     And the payoff is NOT 78ms: the consume II=2 floor over all 3.552M DW input pixels is only
     71ms of DW's 329, so II 2->1 saves AT MOST 35.5ms (10.8% of DW, 4.0% of the network) -- the
     stalls above the II floor don't shrink with II. 35ms for ~0.45ns of margin: no.
  2. 8-byte merge of the two g-lanes' words: needs contiguous targets; 3. they aren't: lane g's
     row base is `out_off + (ci*fpg+g)*out_ch_stride` (`dw_raster_layer.cpp:544-548`), so the two
     lanes sit `out_ch_stride = h_out*w_out` >= 64 bytes apart. Infeasible without changing the
     output layout, which is a network-wide contract (PW reads channel-major). Closed.
  **What the 156ms actually is (fit: 7.45 cycles/output = 29.8 cycles per packed 4-byte write --
  the same ~30-35 cycles/transaction this project back-solved for reads on 2026-09-04): the
  `CROW_CCOL` schedule (`dwr_consume3_Pipeline_CROW_CCOL.verbose.sched.rpt`) puts `writereq`
  (ST_9), `write` (ST_10) and a 5-stage `writeresp` (ST_11-15) in the SAME iteration, so every
  packed write waits for its own B response inside the II=2 pipeline -- ~30 cycles of stall per
  write, 522,240 writes = 156ms.** The lever is therefore handshake COUNT: one `write_request(addr,
  w_out/4)` + one `write_response()` per lane-ROW with the `write(word)`s staying exactly where they
  are in the pipeline -- 79,872 request/response pairs instead of 522,240 (x6.5 fewer), and if the
  ~30-cycle stall is paid once per lane-row: ~24ms instead of 156, **~130ms saving, ~15% of the
  network** -- the largest single lever left anywhere in the design by this arithmetic, and a
  different shape from the failed `PW_WRITEOUT_FLUSH` (that pulled the data writes OUT of the
  pipeline into a serial stage; this leaves them in place and moves only the request/response to
  row boundaries -- but the difference must be verified in the schedule, not assumed: a
  `writeresp` at row end still stalls ~30 cycles once per row, that IS the model). **Open design
  question before any implementation: the two g-lanes.** Two open bursts on one AXI port cannot
  interleave write data (AXI requires data in AW order), and fpg=2 lanes produce words at the same
  pixels -- 4 of 25 real DW layers (11% of DW outputs). Options: row-buffer lane 1 (<= 64 bytes)
  and issue its burst after lane 0's row closes; or restrict the row-burst form to fpg=1 (89% of
  outputs) -- via a compile-time/template split, NOT a runtime `if(fpg==1)` in the pipeline (this
  file's own runtime-gate rule; and `DWR_ENABLE_FPG_SPECIALIZATION`'s own history). Not decided.
  **Verdict on the round's question: DW is not "at its ceiling" -- but the way forward is fewer
  write handshakes inside the existing pipeline, not a second port. PW (55%) remains the largest
  operator; this DW lever (~130ms) is comparable in size to anything visible on PW right now.**
  **STEP 1 DONE, 2026-09-14 -- `DWR_ROWBURST` mechanism probe (OFF by default, inside
  `DW_OUTPUT_BURST`; `run_csynth_rowburst_probe.tcl`, `run_csim_rowburst_probe.tcl`): the schedule
  has the RIGHT shape -- request/response left the iteration body, `write(word)` stayed in the
  pipeline at II=2. NOT the `PW_WRITEOUT_FLUSH` shape. Proceed to step 2.** The probe: one
  `write_request(row_addr, w_out/4)` per lane in a tiny loop BEFORE `CCOL`, `write(word)` unchanged
  inside `CCOL`, one `write_response()` per lane in a tiny loop AFTER `CCOL`; row validity mirrors
  `dwr_produce`'s `row_phase` logic (needs `S`, `w_out` passed in); row base is an accumulator.
  Schedule reports (`rowburst_probe/s1/.autopilot/db/*.verbose.sched.rpt`), counted per module:
  (1) `writereq`: ONLY in `dwr_consume3_Pipeline_VITIS_LOOP_623_5` (the per-row request loop, trip
  2, 6 cycles); `writeresp`: ONLY in `..._VITIS_LOOP_709_10` (the per-row response loop, trip 2,
  the 5-stage op at ST_2-6) -- ZERO of either inside `Pipeline_CCOL`. (2) `write`: 2 ops (one per
  lane) inside `Pipeline_CCOL` at ST_10/ST_11; `CCOL` achieved II=2 (same `200-880` as before, two
  writes per iteration -- unchanged structural floor), iteration latency 15 -> 10 (the 5-stage
  `writeresp` left the body). (3) `write` did NOT leave the pipeline. Cost of the shape:
  `CROW_CCOL` is no longer flattened (`CROW` is now a sequential outer loop around three small
  pipelined loops) -- per row ~20 cycles of request-loop + `CCOL` fill/drain + response-loop
  overhead over all 97,344 DW rows (sum of cin*h_pad) = ~19.5ms, plus the B-wait once per valid
  lane-row (~30 cycles x 72,192 valid rows = ~21.7ms, fpg=1 basis) -- ~41ms of new per-row cost
  against the ~156ms of per-write waits it removes: net ~-115ms expected, not -130. csim sanity (raster tb,
  `-DDWR_ROWBURST`): the three fpg=1 cases PASS byte-exact (confirms request/write/response
  counting per row), the two fpg=2 cases FAIL (59385/98304, 23762/49152) -- exactly the two-open-
  bursts interleaving problem, by construction, step 2's question.
  **Step 2 recommendation, read off this schedule (not decided):** the request and response already
  live in tiny per-row loops OUTSIDE `CCOL`, so the most natural fpg=2 fix in THIS structure is a
  third such per-row loop: lane 1's words go into a small per-row buffer (`ap_uint<32>[16]` max;
  fpg=2 layers have w_out <= 32 -> <= 8 words) inside `CCOL` instead of the port, and after `CCOL`
  a drain loop issues lane 1's request, writes the buffered words, then both responses -- AW order
  is lane 0 then lane 1, data order matches, no interleaving. The buffer's state lives entirely
  within one `CROW` iteration (reset per row; ordinary loop-carried, not DATAFLOW cross-region
  state, which is where this line's three DATAFLOW failures were). The lane split is by unrolled
  lane INDEX (g=0 -> port, g=1 -> buffer), a compile-time distinction, so there is no runtime
  `if(fpg==1)` on the hot path either -- it covers 100% of outputs with one code path. The
  fpg=1-only alternative (runtime path select, keep the per-word form for fpg=2) covers 89% and
  adds a runtime-gated region; not preferred on this schedule's evidence, but it is the user's call.
  **FOLLOW-UP, 2026-09-08, same investigation: `wbuf`'s own II Violations (3 of the original 5)
  were fixed cheaply and cleanly (`#pragma HLS ARRAY_PARTITION variable=wbuf complete dim=1`, +0.21%
  LUT, zero BRAM/DSP, csim clean 5/5+4/4+8/8) -- but achieved II stayed at 8, not 7.** Re-running
  csynth after the fix showed all 5 remaining violations were now `gmem_act` (previously only 2 of
  5), confirming `gmem_act` was independently sufficient to demand the full II=8 the whole time --
  `wbuf` was a second, separately-fixable bottleneck that happened to sit at the same ceiling, not a
  partial contributor whose removal should have dropped II proportionally. **New standing lesson:
  when multiple II-violation sources coexist in one diagnostic, eliminating one does NOT necessarily
  reduce achieved II -- a source that appears at only SOME attempted-II levels may simply be masked
  by another source's earlier failure at those same levels, not because it's a minor contributor.
  Re-check the full violation list after each fix, don't assume proportional improvement.**
  **DECISIVE FOLLOW-UP, same day: pulled the actual scheduling report (`.verbose.sched.rpt`) before
  implementing the pre-registered "sequentialize the two lanes' writes" fix, and it refuted the whole
  premise.** `dwr_writeout_impl`'s own "length-4 burst" (confirmed via `burst.xml`'s "Sequential
  write of length 4 has been inferred", `BurstInferredPassed`) is **not a true wide/coalesced AXI
  burst at the RTL level** -- the schedule shows it decomposed into 4 SEPARATE elemental
  `writereq`+`write`+`writeresp` triples (one per byte), each going through the SAME shared,
  single-accept-per-cycle `m_axi` adapter core (Core 111, "Adapter" type, II=1, but per-op latencies
  `writereq`=9/`write`=5/`writeresp`=3 cycles). Counted 8 distinct `gmem_act` addresses computed per
  beat when both g-lanes flush (2 lanes x 4 elements) -- achieved II=8 lines up EXACTLY with "8
  elemental transactions, 1 accepted per cycle," not with "2 lanes x a 4-cycle burst." **This means
  the planned fix (write all of lane g=0's 4 elements, then lane g=1's) would NOT reduce achieved
  II at all -- it only reorders WHEN the 8 elemental transactions happen, not how many the shared
  adapter must accept.** Not implemented -- the arithmetic itself was the stop condition, checked
  BEFORE writing any code, per this project's own established "check before implementing" discipline
  now applied a 3rd time on this same investigation (input-read alignment, wbuf fix, and now this).
  **STANDING LESSON: an HLS diagnostic's own "burst inferred" wording (`BurstInferredPassed`,
  "Sequential write of length N") describes that a LOOP-LEVEL access pattern was recognized as
  burst-*eligible*, not that the RTL scheduler actually emits one wide, coalesced AXI transfer at
  that length -- whether it does is a separate question the SCHEDULING report answers, not the
  burst-inference report. This is another instance of this project's own "tool wording vs. actual
  RTL behavior" class of finding (same family as `export_design`'s stale-cache reuse and `vitis_hls`'s
  misleading exit code) -- before assuming a "burst inferred" message means N elements move in a
  single wide transfer, check the actual per-operation schedule for the real number of AXI
  transactions it produces.**
  **PIVOT, same day: word-packing is the real lever (reduces the elemental-transaction COUNT, the
  actual bottleneck this round's own scheduling-report read identified) -- matches the ELEMWISE_BURST/
  ROW_READ precedent exactly (4 bytes packed into one `ap_uint<32>` write, since `wbuf[DWR_MAX_FPG][4]`
  is already naturally 4-element-sized). Pre-implementation checks (alignment, bit-width) in progress,
  same discipline as the prior two rounds on this investigation -- not yet implemented as of this
  entry.**
  **IMPLEMENTED, 2026-09-08/09 (DW_OUTPUT_BURST) -- csynth result is a clean win on BOTH axes:
  achieved II 8 -> 2 exactly as predicted, AND isolated LUT went DOWN 1,162 (-1.57%), not up.**
  New `dw_out_burst` port (`ap_uint<32>`, 6th burst_maxi on `bundle=gmem_act`) + `dwr_writeout_packed`
  packs `wbuf[g]`'s 4 bytes into one word write, replacing the OLD `dwr_writeout_impl<true>`'s 4
  elemental AXI writes. Alignment verified structurally before implementing (every real DW layer's
  `w_out` is a power of two (8/16/32/64) => `out_ch_stride` always a multiple of 4 => every real
  `(co, write_ptr)` address mod4==0, all 25 layers x every real output channel, zero exceptions;
  the SLOW/remainder path is therefore DEAD CODE on every real dispatched shape, kept only for
  non-network shapes, same convention as WRITEOUT's own slow path). csim clean 5/5 + 4/4 + 8/8.
  Burst confirmed (`ManualBurstInstancePassed`, Length=1, Width=32). Resource delta vs the
  elemwise_burst-only baseline: **LUT 73,929 -> 72,767 (-1,162/-1.57%), FF -324, BRAM/DSP exactly
  flat** -- the new adapter's own ~657 LUT cost was MORE than offset by removing the old path's
  auto-inferred-burst control logic (address tracking + FIFO handling for what HLS reported as a
  length-4 burst but actually scheduled as 4 separate req/write/resp triples). No stop condition
  triggered; the pre-registered elemwise-port-sharing fallback (share one burst_maxi port between
  GELU/ADD and DW, since their use is mutually exclusive) was never needed -- recorded as an untested
  idea for a future round, not evaluated here.
  **NEW STANDING RULE: `HLS 200-885` and `HLS 200-880` are qualitatively different II-violation
  classes and should be read differently.** `200-885` ("Unable to schedule ... due to limited memory
  ports") is a RESOURCE-CONTENTION violation -- it means more ports/partitioning/fewer simultaneous
  accesses could still improve II, i.e. there is headroom left. `200-880` ("Unable to enforce a
  carried dependence constraint (II = N, distance = ..., offset = ...)") is a genuine ORDERING
  dependency -- the operations cannot overlap regardless of resources, so the achieved II is the
  STRUCTURAL MINIMUM for that code shape, not a scheduler compromise. This round's own transition
  demonstrates it concretely: before word-packing, `CROW_CCOL` showed `200-885` on `gmem_act` at
  achieved II=8 (resource contention, 8 elemental writes on a 1-accept-per-cycle adapter -- real
  headroom, which packing then captured); after packing, it shows `200-880` at achieved II=2 (two
  single-word writes to the same port genuinely cannot issue in the same cycle -- no headroom left
  without a second physical AXI port). **On seeing `200-880`, stop trying resource-side fixes
  (partitioning, port widening, access-count reduction) for that specific violation -- they cannot
  help; only a structural change (a genuinely separate port, or removing one of the two dependent
  operations) can.**
  **CORRECTED 2026-09-14 (`DWR_ROWBURST` step 2): `200-880` says "this SHAPE cannot do better" --
  it does NOT say the shape is necessary.** The `CROW_CCOL` II=2 was read (2026-09-09/14) as a
  structural floor needing "a genuinely separate port" -- wrong conclusion from a right diagnostic.
  The carried dependence was between the two g-lanes' bus writes in the SAME iteration; routing
  lane 1's words through a per-row buffer (drained after the row, behind lane 0's burst) leaves one
  bus write per iteration and `CCOL` went to achieved II=1 with no new port at all. The `200-880`
  premise ("two writes per iteration on one port") was a property of the code's lane structure,
  not of the hardware. Before concluding a `200-880` needs a physical resource, ask whether the two
  dependent operations have to be in the same iteration in the first place.
  **REAL P&R, 2026-09-09: STOP-LOSS TRIGGERED -- the mechanism works but this build does not close
  timing. route_design alone came back WNS=-0.418053ns**, outside both this round's own pre-registered
  `-0.2ns` gate for attempting `phys_opt_design` AND this file's own established
  "-0.3 to -0.5ns => needs a real source-level diagnosis, not a phys_opt attempt" band. `phys_opt`
  was therefore NOT attempted (the pre-registered stop condition was honored, not overridden); no
  bitstream deployed, board untouched, still on `mac_array_a3_elemwise_burst`.
  **The striking part: real resources went DOWN on all three axes and timing STILL degraded.**
  LUT 44,272 (83.22%) -> 43,992 (82.69%), delta -280/-0.63%; BRAM 107 -> 106.5 tiles; DSP 59 ->
  59, exactly flat -- yet WNS went from the baseline's own route_design-alone -0.166790ns to
  -0.418053ns, a -0.251ns degradation. Critical path was confirmed IDENTICAL to the pre-existing
  mechanism (`desc_op_type_reg[24]` -> `mul_32s_32s_32_2_1_U1510/buff0_reg/PCIN`, 8.899ns data path,
  59.0% logic / 41.0% route, all top-10 paths the same shape) -- **the new DW write logic appears
  nowhere in the critical path**; the RTL restructuring (removing the old auto-inferred-burst control
  logic, adding a 6th AXI adapter to the same bundle) evidently steered placement worse on that
  already-marginal shared-multiplier path, independent of the aggregate resource win. This is a
  clean, unusually stark instance of this file's own "LUT occupancy percentage does not predict WNS
  direction" rule -- previous instances compared DIFFERENT builds at different occupancies; this one
  is the same design getting strictly CHEAPER on every resource axis and strictly WORSE on timing,
  which is a stronger form of the same finding.
  **10th isolated-vs-real data point**: isolated csynth predicted LUT -1,162 (-1.57%); real P&R
  delivered -280 (-0.63%) -- correct DIRECTION this time (unlike several prior instances), but ~2.4x
  smaller in magnitude. Consistent with this file's own "direction-agnostic unreliable, don't plan a
  budget around the magnitude" framing.
  **Net position: II 8->2 is real and confirmed (csynth), the resource win is real and confirmed
  (real P&R), csim is clean (5/5 + 4/4 + 8/8), the driver registers are wired (offsets read from the
  fresh export, every pre-existing constant verified unshifted one-by-one) -- but the build cannot be
  deployed as-is.** Closing it would need either a source-level fix on the pre-existing
  `mul_32s_32s_32_2_1` shared-multiplier path (a mechanism this file already documents extensively,
  and which has been the binding critical path on nearly every thin-margin build on this line) or
  accepting that the DW II win is not bankable at the current placement margin. Not decided as of
  this entry -- reported, not pursued further, per this project's own one-round discipline.
  **ROOT-CAUSED, 2026-09-10, via the mature diagnostic flow (critical-path report + binding database
  + baseline comparison) -- and the result RULED OUT the fix that was expected to apply.** The
  accumulator-replaces-`index*stride` fix (this project's most-used timing lever, 5+ successful
  applications including PW_WCHUNK's own -2.264ns -> +0.134ns recovery) has **nothing to target
  here**: compared the FULL multiply-operation set in the binding database (`*.verbose.bind.rpt`'s
  own opset, not the summary `.bind.rpt`) between the baseline and this build, in both `dwr_consume3`
  (7 multiplies each, identical shapes -- `co_1`, `mul120_i`, `mul120_1_i`, plus K/bound-related,
  only line numbers shifted) and `run_layer` (7 multiplies each, identical shapes) -- **zero new
  multiply call sites introduced by this round.** `dwr_writeout_packed` receives `base_addr`
  already computed and only does `base_addr >> 2` (a shift, not a multiply), so it adds no
  `index*stride` site at all.
  **The REAL mechanism, and this project's first PRECISE observation of the hidden cost of adding an
  AXI port (previously only the crude datapoint that gmem_act_wide's own 5th master cost ~0.4ns of
  WNS, mechanism unknown): the 6th write port on `gmem_act` widened the store unit's own
  write-request FIFO and deepened its arbitration logic, and that logic landed directly on the
  already-marginal path feeding the shared multiplier.** Direct evidence, not inference: this build's
  critical path routes through `gmem_act_m_axi_U/store_unit_0/fifo_wreq/U_fifo_srl/`**`mem_reg[5][65]`**
  -- 65 bits wide -- while the DWR_INPUT_BURST build's own report (one fewer write port on the same
  bundle) shows **`mem_reg[5][64]`**, 64 bits. The baseline `elemwise_burst` path does not traverse
  the store unit at all. Logic levels went 4 -> 6 (baseline: DSP48E1=1/LUT2=1/LUT6=2; this build:
  DSP48E1=1/LUT4=2/LUT5=2/LUT6=1), and the path SOURCE changed from `run_layer`'s own FSM state
  register to a `desc_op_type` register feeding that FIFO. **[RETRACTED 2026-09-12 -- see the port-reuse entry directly below: the adapter RTL is byte-identical across builds, `[64]`/`[65]` was a bit index, not a width.] Practical rule going forward (WRONG, do not cite): adding an
  m_axi port to an existing bundle is NOT free even when it adds no new master and no new logic of
  its own -- it widens the shared adapter's request-FIFO and arbitration logic, which on a design
  whose critical path already runs near zero margin can cost more timing than the port's own
  functional win. Check the store-unit FIFO width (`mem_reg[N][W]` in the timing report) before and
  after when adding a port to a bundle that already has several.**
  **PORT-REUSE ATTEMPT RUN AND REFUTED, 2026-09-12 -- and the 2026-09-10 "6th port widened the
  store-unit FIFO" root cause above is RETRACTED, with direct evidence.** Passed `out_burst`
  (PW_FLAT's existing write port) into `run_dw_layer_raster` instead of declaring `dw_out_burst`
  (DW and PW strictly mutually exclusive; no new driver register -- the exported `xmac_array_top_hw.h`
  ends at `DW_IN_BURST 0x100`, identical to HEAD's driver). csim clean (5/5 + 4/4 + 8/8); isolated
  csynth: `CROW_CCOL` achieved II=2 preserved, single `200-880`, LUT 72,639 (-128 vs the 6th-port
  build, -1,290 vs baseline); fresh export (`dw_out_reuse_export`, no `dw_out_burst` in the HDL).
  **Real P&R (route_design alone): WNS=-0.461078ns -- WORSE than the 6th-port build's -0.418 and
  the baseline's -0.167.** LUT 43,938/53,200 (82.59%, -334 vs baseline), BRAM 106.5, DSP 59. Outside
  the ~-0.2ns `phys_opt_design` band -> not attempted, per the pre-registered stop; bitstream built
  but NOT deployed, board untouched.
  **Acceptance criterion failed in the most informative way: the timing report STILL shows
  `mem_reg[5][65]` with the port count back at the baseline's value.** Chased that down in the
  exported RTL: `mac_array_top_gmem_act_m_axi.v` is BYTE-IDENTICAL (`diff` = 0 lines) across all four
  builds -- baseline (`elemwise_burst`), DWR_INPUT_BURST (`[64]`), 6th-port (`[65]`), reuse (`[65]`).
  `fifo_wreq`'s `DATA_WIDTH` is a fixed `USER_AW + 32` regardless of how many `burst_maxi`/pointer
  arguments share the bundle -- HLS emits ONE adapter per bundle and its width does not depend on the
  port count at all. `mem_reg[i][j]` in an SRL FIFO is depth-slot i, BIT j: `[64]` vs `[65]` was which
  bit of the same-width FIFO the critical path happened to route through, not a width change. **The
  "adding an m_axi port to an existing bundle widens the shared adapter's request FIFO" rule above is
  wrong and must not be cited; the only real cost of an extra port on a bundle is its own control
  register (the shared-bundle != shared-register trap) and whatever placement perturbation the
  changed netlist causes.** Went one step further to make sure nothing structural was missed: the
  top-level shared 32x32 multipliers' operand-mux blocks (the sink of every thin-margin critical path
  on this line), normalized for unit/line numbers, are ALSO identical between baseline and reuse --
  same arms, same inline `desc_op_type == 4|5` compare on one unit, same registered-predicate arms on
  the other. The baseline's worst path is the FSM-sourced 4-logic-level arm (-0.167); the three
  post-DWR_INPUT_BURST builds' worst path is the `desc_op_type`-sourced 6-logic-level arm (-0.103 /
  -0.418 / -0.461). Both arms exist in every build; logic delay is flat across all four (5.17-5.29ns),
  the whole spread is ROUTE delay (3.42 -> 3.65 -> 3.83ns). **Conclusion: the packed-write builds'
  timing loss is not attributable to any identified RTL change on the critical structures -- it is
  the already-documented "several near-tied critical paths swap places" behavior of this design (see
  the 150MHz sweep entry), with placement variance of roughly +-0.3ns on the shared-multiplier sink
  between builds whose relevant RTL is identical.** The DW II 8->2 mechanism itself is sound and
  cheaper on every resource axis; what blocks it is that the deployed baseline already sits at
  -0.167 route-only on this same sink with zero headroom, so any netlist change is a coin flip on
  this path.
  **Two lessons, both process-level:** (1) a netlist NAME in a timing report (`mem_reg[5][65]`) is
  not a structural claim -- before attributing a timing change to "widened/deepened X," diff the
  generated RTL for X between the builds (a 10-second check that would have refuted the 2026-09-10
  root cause before a full export+P&R round was spent on it); (2) the pre-registered acceptance
  criterion being a specific, checkable netlist fact (not just "WNS improves") is what made this
  round's negative result decisive in one shot instead of ambiguous.
  **RESOLVED same day (2026-09-12), two steps in the order the user set -- gate first, then fix the
  sink ALONE on the clean baseline, so the sink fix is a single-variable measurement:**
  **Step 1 -- `DW_OUTPUT_BURST` gated OFF by default** (`dw_raster_layer.cpp/.h`, `_tb.cpp`,
  `mac_array_raster_integrated.cpp`; the `out_burst_w` parameter, `dwr_writeout_packed`, its call,
  AND the `wbuf` partition pragma -- which on its own never moved II -- all under the one macro, same
  convention as `DWR_INPUT_BURST`). Verified the default build is source-identical to `8868355` (the
  deployed baseline's source) apart from the gated blocks: `git diff 8868355` filtered to non-
  comment, non-`#ifdef` lines shows only lines inside the gated blocks; isolated csynth of the
  default build reproduces the baseline's CROW_CCOL achieved II=8. Disposition recorded in the
  source comment: mechanism correct (II 8->2, all three resource axes down, csim clean in both
  forms), blocked only by the baseline's zero timing margin on the shared-multiplier sink,
  re-evaluate once that margin improves.
  **Step 2 -- `SCALAR_OP_SIZE_HOIST`, real P&R route_design alone: WNS -0.167ns -> +0.138388ns
  (+0.305ns), the first build on this line since `macpd4` to close WITHOUT `phys_opt_design`.**
  The six scalar ops each computed their own element count inside their body (RELU/SIGMOID/GELU/ADD
  `cin*h_in*w_in` = 2 multiplies each, GAP/SCALE `h_in*w_in` = 1 each: 10 multiply call sites, all
  reachable only through the top-level op_type `switch`). Now `scalar_hw = h_in*w_in` and
  `scalar_total = cin*scalar_hw` are computed ONCE, unconditionally, in `mac_array_top` before the
  switch and passed in as `int` parameters (same technique as the earlier d.cin/d.cout -> scalar-
  parameter change). Judgment in the pre-registered order: (1) binding DB (`mac_array_top.verbose.
  bind.rpt`): the top's Multiplier opset went from 8 op_type-predicated size multiplies (`mul_ln1934`/
  `total` @`op_type==4`, `mul_ln1965`/`total_5` @`op_type==5`, `HW`, `HW_1`) to exactly two
  `Predicate = true` ops (`scalar_hw`, `scalar_total`) -- zero `desc_op_type` predicates on any
  multiplier op; `run_gelu`/`run_add`'s own bind reports went 4 -> 0 Multiplier ops each; in the
  exported RTL the shared 32x32 multiplier's operand mux went from 4 arms (run_layer, run_gelu, the
  inline `desc_op_type == 4|5` compare arm, a registered-predicate arm) to 2 (run_layer, the
  unconditional `w_in*h_in`), and one whole 32x32 multiplier instance disappeared (4 -> 3 units).
  (2) csim 5/5 + 4/4 + 8/8 clean (`run_csim_sohoist_all.tcl`, all three real testbenches in one
  session). (3) WNS above. (4) Real resources: LUT 44,054/53,200 (82.81%, -218 vs baseline's
  44,272), BRAM 107 (flat), DSP 56 (-3 vs 59) -- down, as expected from fewer multiply sites.
  **Where the margin came from, from the critical-path report, not inferred**: same sink
  (`mul_32s_32s_32_2_1`), same source shape (`run_layer`'s FSM state, 4 logic levels), but data path
  8.707 -> 8.251ns: LOGIC 5.287 -> 5.097 (one LUT6 in the operand mux became a LUT4 -- the mux is
  narrower with 2 arms instead of 4) AND ROUTE 3.420 -> 3.154 (fewer arms, a smaller cone to place).
  I.e. the improvement is in the sink's own structure, which is exactly why it should transfer to
  future rounds instead of being another placement roll. Per the pre-registered decision rule
  (">=0.2ns re-opens DW_OUTPUT_BURST; ~0.05ns means the bottleneck is not the operand side"), this
  RE-OPENS `DW_OUTPUT_BURST` as the next single-variable round on top of this build. **Coverage gap
  closed and build PROMOTED the same day (see the deployed-baseline section)**: none of the three csim
  testbenches exercised RELU/SIGMOID/GAP/SCALE (four of the six functions whose signature this round
  changed) -- added `scalar_ops_real_desc_tb.cpp` (real `desc_all.bin` descriptors for entries
  75/77/79/80, all 28 fields and real offsets into one shared arena; reference computed independently
  in Python by `tools/gen_scalar_ops_csim_bundle.py`, replicating the HARDWARE's arithmetic -- the
  placeholder `clamp(x+64,0,127)` sigmoid, truncate-toward-zero GAP division, arithmetic-shift clip --
  not the mathematical functions; verified the test has teeth: 397/768 GAP channels would fail under
  floor division, SIGMOID hits both clamps). 4/4 csim, then the same data as board bundles
  (`board_test_scalar_ops/`, offsets relocated for the single-op driver): byte-exact on the prior
  baseline bitstream (control) AND on this build. Note the four old `build_single_op_test_entry75_gap
  .py`-style builders were never used for this -- they are among the 18 still-stale 27-field scripts
  and their `ref_out` comes from the questionable `entry_NN.bin` csim dumps.
  **Lesson (adds to the "several near-tied arms swap places" record above): when a design's
  critical path is a SHARED functional unit whose operand mux is fed from N call sites, the number
  of arms in that mux is itself a timing lever independent of any one arm's own logic depth --
  reducing call-site count (hoisting a computation to one unconditional site) shrank BOTH the logic
  and the route delay on the sink. Check the sink's mux arm count in the exported RTL (the
  `always @(*)` block driving `grp_fu_NNN_p0/p1`) before assuming the margin is "just placement."**
  **ARM INVENTORY OF THE SINK, 2026-09-12 (analysis round on the deployed `sohoist` build, no
  build; binding DB + exported RTL + a 300-path timing report -- the pre-registered prerequisite
  before deciding between "clean more arms" and "re-open DW_OUTPUT_BURST"):**
  The critical sink `mul_32s_32s_32_2_1_U1485` is fed by a 2-arm top-level mux (`run_layer`'s
  forwarded operand @state72; `scalar_hw = w_in*h_in` @state2) -- but `run_layer`'s arm is ITSELF
  a 6-arm mux (`grp_fu_1281`, FSM states 86/88/101/103/105/122), so the sink effectively has 7
  sources, and the worst path's source `ap_CS_fsm_reg[121]` IS state 122:
    1. state122 -- `pw_flat_pipeline_impl<false>`'s `mul_ln579 = (rt*MAC_PR + wr_row) * d.w_out`
       (line 579), INSIDE the `PW_FLAT` II=1 pipeline's writeout (`Predicate = in_writeout &
       icmp_ln542`), one multiply per writeout iteration, bound OUT of the pipeline onto the shared
       unit via an external FU port. **This is the arm on the critical path.** The `<true>` instance's
       identical multiply is bound to a second, `run_layer`-local unit (`U1439`, 2 arms: it and #7).
    2. state105 -- `oh * W` (line 1133, `ROW_READ` byte address, once per (rt,rr,ci)).
    3. state103 -- `pw_ot_count * d.cin` (line 1846, per-chunk accumulator step).
    4. state101 -- `pw_total_iters = pw_ot_count * (n_cbase*32+16)` (line 1041, per chunk).
    5. state88  -- `w_total = d.cin * pw_ot_count` (line 1028, per chunk) -- the SAME product as #3;
       HLS did not CSE them across the loop.
    6. state86  -- `pw_ot_lo = wchunk * pw_ot_per_chunk` and a `cin * (~x)` term from the same
       line-1023 expression chain (per chunk).
    7. (on `U1439`) `pw_ot_count * d.out_ch_stride` (line 1847, per chunk).
  `scalar_total = cin*hw` already has its own dedicated unit (`U1484`, no mux).
  **Every one of the 7 is eliminable without a multiplier**: #1 = rt-accumulated row base passed
  into `pw_flat_pipeline_impl` + a 4-entry `wr_row*w_out` table built with 3 adds -- NOTE this exact
  conversion was ATTEMPTED AND REVERTED on 2026-08-25 (see the comment above line 579) because it
  "did not change `mul_32s_32s_32_2_1`'s real INSTANCE count (still 2)" -- that was the wrong
  metric: instance count stays 2 as long as ANY arm remains; the right metric is the arm count on
  the sink's mux / the binding DB's opset (this session's finding), and by that metric the reverted
  conversion removes precisely the critical arm. Re-do it, judge it by arm count + WNS. #2 = row
  accumulator (`+= W` per row, rt base stepped by `W<<2`). #3/#5/#4/#7 = one small add-loop per
  chunk accumulating `cin`, `out_ch_stride` and `iters_per_ot` over `pw_ot_count` iterations (<=768
  adds x <=3 chunks per layer, ~7-23us -- negligible vs ms-scale layers, but nonzero, flag it) --
  or at minimum reuse `w_total` for #3 (zero cost, removes one arm outright). #6 = `pw_ot_lo +=
  pw_ot_per_chunk` accumulator. Top-level `scalar_hw`/`scalar_total`: give them NARROW operand
  types (`h_in`,`w_in` <= 256 -> 9 bits; `cin` <= 1152 -> 11 bits) so HLS emits a different, narrow
  multiplier core that cannot share with the 32x32 unit -- the top-level mux then disappears
  entirely instead of needing an `ALLOCATION` pragma (which this project has seen silently ignored).
  **Expected gain, and its ceiling (the honest answer to "how much does 2 arms -> 0 buy"):** the
  300-path report on the `sohoist` checkpoint shows the sink at +0.138 (PCIN) / +0.230 (operand
  register), and the next-worst DISTINCT structure at **+0.246ns**: DW's `dwr_*` -> `gmem_act`
  store-unit `fifo_wreq` (9 logic levels, 7.81 of 9.44ns ROUTE -- a placement-distance signature);
  then +0.450 (`gmem_w` load buffer -> DW gather, 7 levels, 8.9ns route). So removing the sink
  outright moves WNS to about +0.25 at this placement -- a ceiling of roughly **+0.11ns**, NOT
  another +0.3: the 4->2 round's gain was large because the sink was the only thing near zero; the
  next structures are already within 0.1-0.3ns of it. What the cleanup buys that the number
  understates: the ONE structure that has been the critical path on every thin-margin build of this
  line (baseline, dummy4th, elemwise_burst, DWR_INPUT_BURST, both DW_OUTPUT_BURST forms) stops
  existing, so its +-0.3ns placement variance stops applying. **For DW_OUTPUT_BURST specifically:
  the two next-worst paths are DW's own writeout side and the `gmem_act` store unit -- exactly the
  structures DW_OUTPUT_BURST modifies -- so its outcome after the cleanup is genuinely a coin flip
  around 0 (+0.25 ceiling minus whatever it perturbs), not a prediction; but it is a DIFFERENT coin
  than last time (route-dominated DW/store paths, not the shared multiplier).** Order recommended:
  arm cleanup as one round (one mechanism -- call-site elimination on one FU -- several edits,
  judged by the binding DB showing ZERO Multiplier ops in `run_layer` and the sink absent from the
  top-10), THEN DW_OUTPUT_BURST on top of it.
  **ARM CLEANUP RUN, 2026-09-12 (`SHARED_MUL_ARMS`, real P&R, no board): the sink is GONE as
  designed, and WNS did not move -- +0.128ns vs +0.138 -- because a path that was not even in the
  previous build's top-300 came out worst at the new placement.** All seven conversions built
  (line-579 row product -> `rt_row_base` + `wr_row_off` accumulators; `oh*W` -> `rt_in_base` +
  `rr_w`; `pw_ot_lo` accumulator; the four per-chunk `pw_ot_count` products and the top-level
  `scalar_hw`/`scalar_total` -> NARROW-typed multiplies). csim 5/5 + 4/4 + 8/8 + 4/4. Binding DB
  (criterion 1): `run_layer` has zero 32x32 multiply ops, the top has zero; `mul_32s_32s_32_2_1`
  exists only inside DW's own `dwr_consume3` (2 instances, unchanged, dedicated); `run_layer`'s 4
  remaining multiply ops are the narrow per-chunk products (i21/i22) on one 1-DSP narrow unit. Real
  P&R route_design alone: **WNS +0.128041ns**, LUT 44,622 (83.88%, +568 -- isolated had said -283;
  the narrow `mul_*_1_1` cores are LUT-fabric multipliers), BRAM 107 flat, DSP 46 (-10). Criterion 2
  met: the shared unit is absent from the top-10 (only 32x32 path in the top-300: DW's own, at
  +0.395, 2 logic levels). **New worst path: `dwr_produce2`'s `ROW_COL` pipeline, `select_ln209_reg
  -> read_ptr[4]/CE`, 13 logic levels (CARRY4x9), 9.138ns with 5.53ns ROUTE -- absent from the
  `sohoist` build's entire top-300.** Behind it: +0.241 (`pw_flat_pipeline_impl<true>` -> the
  `LayerDescV2` descriptor RAM, 6 levels, 80% route), +0.264/+0.310 (`gmem_w` load buffer -> DW
  gather, the +0.450 path from before, now worse), +0.395 (DW's own multiplier). The +0.246 DW ->
  `gmem_act` store-FIFO path that bounded the prediction is not in the new top-8 at all.
  **Reading: the pre-registered "+0.18 would mean the store-unit path bites first" case happened in
  a stronger form -- the floor is not one specific next path, it is a POPULATION of route-dominated
  paths (DW producer carry chain, descriptor RAM reads, `gmem_w` -> DW gather, DW -> store FIFO) all
  within ~+0.13..+0.45 of zero at 10ns, and any placement lands one of them at ~+0.13 +- 0.1. The
  shared-multiplier sink is permanently gone (a real structural result: no future netlist change
  can re-trigger its +-0.3 swing), but it was not the floor -- it was the first of several
  near-tied structures. This is the SAME "near-tied paths swap places" record, third confirmation,
  now with the previous #1 removed and the population still there.**
  **NEW HLS LESSON from this round: an add-loop is NOT a way to avoid a multiplier.** The first
  form of the per-chunk fix (one `PIPELINE II=1` loop accumulating `cin`, `out_ch_stride`,
  `iters_per_ot` over `pw_ot_count` iterations) was rewritten by HLS's loop-idiom pass back into
  three i32 multiplies on the same shared unit (binding DB: `mul_ln1056/_1/_2`, predicate
  `icmp_ln1056`) -- "add a loop-invariant N times" is recognised as `N*c`. The accumulator technique
  only works when the accumulated quantity is genuinely loop-carried across REAL work (a row base
  stepped inside the row loop), not a standalone summation loop. What did work, both here and at
  the top level: narrow operand types (`ap_uint<11>` x `ap_uint<15>` etc.), which force a
  different multiplier core (`mul_11ns_15ns_26_1_1`) that cannot be bound onto `mul_32s_32s_32`
  -- verified in the binding DB and the exported module list, both times.
  **Disposition: source kept (not gated -- it is a strict structural cleanup with csim clean, P&R
  closed, no regression, and it removes the sink permanently), NOT promoted (same margin as the
  deployed `sohoist`, +568 LUT, no board round run), working tree is one step AHEAD of the deployed
  bitstream. DW_OUTPUT_BURST re-test is the pre-registered next round; whether to run it on this
  source (sink gone; its own writeout side + `gmem_w`/store paths are now the ones near zero) is
  the decision point.**
  **DW_OUTPUT_BURST RE-TEST ON TOP OF SHARED_MUL_ARMS, 2026-09-13 (`-DDW_OUTPUT_BURST`, port-reuse
  form, real P&R, no board yet): route_design alone WNS = +0.172093ns -- CLOSED, better than the
  deployed `sohoist` (+0.138) and than `sma` alone (+0.128).** Pre-registered order held: (1)
  csynth first -- isolated LUT 72,090 (-1,418 vs sma), `CROW_CCOL` achieved II=2 with the single
  `200-880`, no 32x32 unit in `run_layer`/top, register map unchanged (last register `DW_IN_BURST`
  0x100; the reuse form needs no new driver register); the real-LUT question was answered from this
  mechanism's own two prior real deltas (-280, -334, both negative, ~1/4 of isolated) -> expected
  ~44,300, below the 85% "this is a resource problem" line, so P&R was run; (2) csim 5/5 + 4/4 +
  8/8 + 4/4; (3) WNS above. Real utilization: LUT 44,080/53,200 (82.86%, **-542 vs sma's 44,622**
  -- the mechanism's real delta was negative a third time, larger than before), BRAM 106.5, DSP 46.
  Worst path: `gmem_act` load unit's read-data buffer (`buff_rdata/dout_vld_reg -> raddr_reg`, 9
  logic levels, 7.41 of 9.57ns route); then +0.178 (descriptor register CE), +0.238/+0.316 (DW's
  own consume / `gmem_w` -> DW gather). The store-unit `fifo_wreq` paths are in the top-300 (156
  mentions) but not the top-6 distinct. **This is the "different coin" outcome, and it came up
  heads: the two builds that killed this mechanism (-0.418/-0.461) both died on the shared
  multiplier sink; with that sink structurally removed, the SAME mechanism, same source form,
  closes with margin to spare, on a placement whose worst path is just another member of the
  route-dominated population.** Third real P&R of this mechanism, first to close. Not board-tested
  this round (the round's pre-registered scope was csynth/csim/P&R) -- the DW II 8->2 win (predicted
  from `CROW_CCOL`'s 8 -> 2, i.e. DW's ~531ms should drop substantially) is a BOARD claim and is
  not made here. Source state: `DW_OUTPUT_BURST` is still OFF by default in the source; this build
  was made with `-DDW_OUTPUT_BURST` on the command line (`run_export_ip_dwob2.tcl`,
  `run_csim_dwob2_*.tcl`) -- flipping the default is part of the promotion decision, not done.
  **BOARD ROUND DONE AND PROMOTED, 2026-09-13 -- see the deployed-baseline section
  (`mac_array_a3_dwob`): DW 531.13 -> 329.0ms (-38.0%), full network 1,095.36 -> ~898ms (-18%),
  everything byte-exact, ONNX cosine exact, both inside the pre-registered intervals (250-400 /
  820-980). `DW_OUTPUT_BURST` default flipped ON (`dw_raster_layer.h`, `DW_OUTPUT_BURST_OFF` to
  revert), flag-less build verified identical to the tested one.**
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
  **GENERALIZED RULE, not specific to `ckpt_hw_*`: before trusting ANY persisted reference/golden data
  file as a correctness judge, check its generation timestamp against the date of the last real
  architecture change to whatever it's supposed to be validating -- if the reference predates that
  change, it cannot prove anything about post-change correctness no matter how clean the comparison
  looks.** This is the same underlying failure class this file already documents for other persisted
  artifacts (the stale Stem-calibration scale, the wrong-resolution default image) -- reference/golden
  data doesn't announce its own staleness, and a clean-looking pass/fail number gives no signal about
  whether the file being compared against is itself current. **Correction check performed 2026-09-02:
  swept the other two commits with real full-network checkpoint claims made between the reference's
  last generation (2026-08-23) and this correction (`000350a` DW raster integration, 2026-08-28;
  `32a6f1d` PW_FLAT II fix, 2026-08-27) -- both turned out NOT to need correction. Read closely, both
  commit messages explicitly describe a same-session A/B comparison (fresh board dump from the OLD
  build vs. fresh board dump from the NEW build, both real hardware, neither compared against
  `ckpt_hw_*` or any golden file at all) -- "7 checkpoints... IDENTICAL byte-for-byte between both
  runs," not "byte-exact vs csim." Only `mac_array_a3_gmemmeta_elim1`'s own claim (explicitly "vs
  csim (cosine=1.000000)") was actually vulnerable and needed the correction added above. **Don't
  assume every historical checkpoint claim in this project's history is equally suspect just because
  one class of them was -- check what each one was actually compared against before correcting it.**
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
