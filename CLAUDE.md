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

## Hard stop list (do not do these unless a session explicitly says the phase has changed)

- No more 200MHz P&R on the **old** architecture (op_code+shared-m_axi) — exhausted across dozens of
  rounds (ZHR-11 Tier A x10, ZHR-12 Tier B x4 granularities, ZHR-16 x6 rounds).
- No more placement/phys_opt directive rotation as a timing lever — falsified 4 separate times.
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
- All results — including negative ones — get written back to the relevant Linear issue as a
  comment, not just left in chat or local memory. Real numbers over assumptions: this project has
  been burned before by static-report/simulation readings that turned out wrong (ZHR-5's "140x
  mystery" — 6 of 8 leads were misled by static analysis, only real bitstream sweeps found the true
  cause). When a question can be answered by running real hardware instead of reasoning about code,
  run the hardware.
- Board safety: the currently-deployed bitstream is the golden rollback image — never overwrite it
  without being told to. Any new binary/bitstream gets a small isolated test before a full-network
  run (ZHR-10: a change that was "HLS/Vivado all-green" hung the real board).

## Known open issues as of 2026-08-13

- **Accuracy**: end-to-end cosine similarity (board vs ONNX float32 reference) measured at ~0.47,
  far below this project's own ≥0.99 target (DEPLOY_PLAN.md §5.3). The deployed bitstream's numeric
  output is measurably wrong, not just a theoretical risk. Most likely cause: a real, confirmed
  sigmoid LUT indexing bug in `fastvit_infer.c`'s `se_block()` (`(uint8_t)ex[co]` should be
  `(uint8_t)(ex[co]+128)`) — not yet fixed or re-tested. See Linear ZHR-8 and ZHR-63 for full
  writeup, and `tools/run_accuracy_harness.py` / `tools/compare_accuracy_results.py` to reproduce.
- **FinalDW zero-output**: under a specific degenerate synthetic input (arithmetic ramp), the
  `FinalDW` layer's output collapses to a hard zero — but this was NOT reproduced across 5
  progressively-more-realistic isolated hardware tests (including real addresses/weights/bias/shift
  at full scale), and does NOT occur with realistic image-like inputs (84% non-zero output). Likely
  an input-pattern-specific edge case, not a general hardware correctness bug — unconfirmed, see
  ZHR-8 for the full investigation and handoff notes.
- **git**: this repo had no version control until 2026-08-13. The initial commit is a single
  consolidated snapshot (no prior VCS existed to replay), with annotated tags pointing at real
  preserved historical source (`fastvit_ip_v1.2_backup/`, `dwconv_worker.tile_backup_2530ns.cpp`,
  etc.) — see `git tag -l -n99` for a navigable timeline and each tag's message for what it actually
  represents.
