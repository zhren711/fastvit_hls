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
- DSP-packing is deferred, not rejected — old rejections assumed an LUT-bound chip, true only because
  AXI glue was eating the LUT budget in the old architecture. Revisit in Phase C/D, not before.
- 125/142.86MHz frequency midpoints: a cheap side-check during Phase A at most, never a mainline goal.

## Working method (non-negotiable, not stylistic)

- **One round = one hypothesis + one measurement + one conclusion.** Report the result and stop —
  do not chain straight into the next round without a human checkpoint. This is not a suggestion:
  ZHR-16's round 3→4→5→6 ran back-to-back with no checkpoint and the user identified that as the
  reason the project stalled for two weeks. If a debugging chase runs past 2-3 rounds without a
  natural conclusion, stop and check in even if each individual round felt justified in the moment.
- If a "fix one bottleneck, the next equally-bad one pops up" pattern appears, STOP and name it as an
  architectural pattern — do not keep fixing individual instances.
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
- **A runtime-derived expression used as a loop bound becomes real hardware arithmetic (often a
  multiplier), not just a comparator.** Confirmed by direct measurement, not inference: converting
  four staging loops from runtime bounds (`patch_r`/`patch_c` = `(MAC_PR-1)*S+K`, `K` itself) to
  compile-time constants predicted a DSP drop from csynth (90→63, -27) that real P&R matched exactly
  (63). The loop-bound expressions themselves — not just the write-enable logic those bounds gated —
  were being synthesized into hardware every time they appeared in a loop's exit condition. Any
  derived-bound expression (`(dim-1)*stride+k`-shaped or similar) sitting in a loop bound is a
  candidate for this, independent of whether the loop body writes into a partitioned register array.
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
- All results — including negative ones — get written back to the relevant Linear issue as a
  comment, not just left in chat or local memory. Real numbers over assumptions: this project has
  been burned before by static-report/simulation readings that turned out wrong (ZHR-5's "140x
  mystery" — 6 of 8 leads were misled by static analysis, only real bitstream sweeps found the true
  cause). When a question can be answered by running real hardware instead of reasoning about code,
  run the hardware.
- Board safety: the currently-deployed bitstream is the golden rollback image — never overwrite it
  without being told to. Any new binary/bitstream gets a small isolated test before a full-network
  run (ZHR-10: a change that was "HLS/Vivado all-green" hung the real board).
- When the code itself contains an admitted placeholder/TODO (a hardcoded stand-in value, a comment
  saying "not yet calibrated"/"not yet implemented", etc.) and the observed symptom is consistent with
  that placeholder being the cause, verify the placeholder first — before chasing a more interesting
  or more specific-sounding hypothesis. `calibrate_activations.py`'s `default_act_scale=1/127` was
  flagged as Phase 0.7's own step 3 at kickoff ("replace the placeholder with real calibration") and
  then deferred through 9+ debugging rounds while more specific theories (SE `out_shift`, a missing
  final GELU, LayerScale) got chased instead — it turned out to be the dominant root cause, off by
  ~37x, confirmed only in Phase 0.8 step 5 by finally checking it directly.

## Known open issues as of 2026-08-15

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
