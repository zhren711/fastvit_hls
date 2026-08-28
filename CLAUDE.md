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
