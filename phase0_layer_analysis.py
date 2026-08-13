#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Phase 0.3/0.4 analysis: parse the real v18gelu per-layer [LayerTiming] log
(phase0_evidence_run1_stderr.log, captured 2026-04-08 right after v18gelu
was flashed, board untouched since) into a per-layer ms table, and compute
each layer's theoretical MAC cycles + utilization against the DEPLOYED
engines' real unroll/pipeline parallelism (from fastvit_ip/{conv,dwconv,
pwconv}_worker.cpp, fastvit_ip.h):

  CONV   engine: CONV_TM=2 x CONV_TN=2      = 4  MACs/cycle
  DWCONV engine: DW_TC=4 (DW_TN=1, 1 ch/cyc) = 4  MACs/cycle
  PWCONV engine: PW_TM=8 x PW_TN=8           = 64 MACs/cycle

Clock = 100MHz (deployed v18gelu frequency).

Layer sequence/shapes are taken directly from
petalinux/software/fastvit_app/src/fastvit_infer.c (the v5 "ALL operators
on FPGA" source actually linked into the deployed fastvit_infer_v18gelu
binary) -- including the 3 PRUNED PW-expand layers (Stage2 blk0: 288->240,
Stage3 blk1: 576->480, Stage4 blk1: 1152->960; see tools/export_weights_pruned.py
comment in that file).
"""
import re

LOG_PATH = "phase0_evidence_run1_stderr.log"

# ── Parse the log: use the SECOND run (the timed one, not warmup) ────────
with open(LOG_PATH, "r", encoding="utf-8") as f:
    lines = [l.strip() for l in f if l.strip()]

entries = []  # (cum_ms, label)
for l in lines:
    m = re.match(r"\[LayerTiming\]\s+([\d.]+)\s+ms\s+(.*)", l)
    if m:
        entries.append((float(m.group(1)), m.group(2)))

# split into runs at each "start" marker
runs = []
cur = []
for ms, label in entries:
    if label == "start" and cur:
        runs.append(cur)
        cur = []
    cur.append((ms, label))
if cur:
    runs.append(cur)

print(f"Found {len(runs)} run(s) in log; using the LAST (timed) run.")
run = runs[-1]

# per-line delta ms (this IS the real per-op wall-clock time on real hw)
#
# IMPORTANT semantics fix: fastvit_infer.c calls TSTEP_FMT(label) BEFORE
# fv_run_*(), i.e. each printed line's cumulative timestamp marks the
# START of the operation it names, not its end. So op `i`'s real duration
# is entries[i+1].ms - entries[i].ms (the gap to the NEXT timestamp), not
# entries[i].ms - entries[i-1].ms. Pairing it the naive backward way (as
# an earlier version of this script did) silently shifts every duration
# onto the WRONG label and produces nonsense >100% utilization numbers
# for the tiny "marker"/dispatch-gap lines. entries[0]="start" has no
# row of its own (rows[] starts at "Stem: Conv..."), and the final
# "done" entry has no next timestamp to diff against, so it gets 0.
deltas = []
for i in range(1, len(run) - 1):
    ms_cur, label = run[i]
    ms_next, _ = run[i + 1]
    deltas.append((label, ms_next - ms_cur))
deltas.append((run[-1][1], 0.0))  # "done" -- no real duration

# ── Engine peak MACs/cycle (real unroll factors, see docstring) ──────────
CLOCK_HZ = 100e6
PEAK = {"CONV": 4, "DW": 4, "PW": 64}

def dw_macs(chout, h, w, k):
    return chout * h * w * k * k

def pw_macs(chin, chout, h, w):
    return chin * chout * h * w

def conv_macs(chin, chout, hout, wout, k):
    return chin * chout * hout * wout * k * k

# ── Hardcoded shape table, in EXACT call order matching
# fastvit_infer.c's fastvit_t8_infer() (petalinux v5 source). Each row:
# (log_label_substring, engine, macs, note)
# GELU/Add/"done"/block-marker lines carry 0 MACs (real HW time, but not
# MAC-producing) and are kept in the table for completeness.
rows = []

def add(label_sub, engine, macs, note=""):
    rows.append((label_sub, engine, macs, note))

# Stem
add("Stem: Conv3x3 3->48 s=2",      "CONV", conv_macs(3, 48, 64, 64, 3))
add("Stem: DW3x3 48 + GELU",        "DW",   dw_macs(48, 64, 64, 3))
add("Stem: PW 48->48",              "PW",   pw_macs(48, 48, 64, 64))
add("Stem: DW3x3 48 (no act)",      "DW",   dw_macs(48, 64, 64, 3))

def repmixer(prefix, has_dw3, c, h, w, cexp, tag):
    if has_dw3:
        add(f"[{tag}] DW3 C={c} {h}x{w}",  "DW", dw_macs(c, h, w, 3))
        add(f"[{tag}] DW7 C={c} {h}x{w}",  "DW", dw_macs(c, h, w, 7))
    else:
        add(f"[{tag}] DW7-only C={c} {h}x{w}", "DW", dw_macs(c, h, w, 7))
    add(f"[{tag}] PW1 {c}->{cexp} {h}x{w}", "PW", pw_macs(c, cexp, h, w))
    add(f"[{tag}] GELU",                    "GELU", 0)
    add(f"[{tag}] PW2 {cexp}->{c} {h}x{w}", "PW", pw_macs(cexp, c, h, w))
    add(f"[{tag}] Add C={c} {h}x{w}",       "ADD", 0)
    add(f"[{tag}] done",                    "MARK", 0)

# also account for the block-entry marker line ("Stage1 blk0 RepMixer...")
def block_marker(label_sub):
    add(label_sub, "MARK", 0)

block_marker("Stage1 blk0 RepMixer C=48 64x64")
repmixer("S1B0", False, 48, 64, 64, 144, "S1B0")
block_marker("Stage1 blk1 RepMixer C=48 64x64")
repmixer("S1B1", True, 48, 64, 64, 144, "S1B1")

add("Trans1 FPGA DW7 fpg=2 48->96 64->32", "DW", dw_macs(96, 32, 32, 7))
add("Trans1 PW 96->96",                     "PW", pw_macs(96, 96, 32, 32))

block_marker("Stage2 blk0 RepMixer C=96 32x32")
repmixer("S2B0", True, 96, 32, 32, 240, "S2B0")  # PRUNED 288->240
block_marker("Stage2 blk1 RepMixer C=96 32x32")
repmixer("S2B1", True, 96, 32, 32, 288, "S2B1")

add("Trans2 FPGA DW7 fpg=2 96->192 32->16", "DW", dw_macs(192, 16, 16, 7))
add("Trans2 PW 192->192",                    "PW", pw_macs(192, 192, 16, 16))

block_marker("Stage3 blk0 RepMixer C=192 16x16")
repmixer("S3B0", True, 192, 16, 16, 576, "S3B0")
block_marker("Stage3 blk1 RepMixer C=192 16x16")
repmixer("S3B1", True, 192, 16, 16, 480, "S3B1")  # PRUNED 576->480
block_marker("Stage3 blk2 RepMixer C=192 16x16")
repmixer("S3B2", True, 192, 16, 16, 576, "S3B2")
block_marker("Stage3 blk3 RepMixer C=192 16x16")
repmixer("S3B3", True, 192, 16, 16, 576, "S3B3")

add("Trans3 FPGA DW7 fpg=2 192->384 16->8", "DW", dw_macs(384, 8, 8, 7))
add("Trans3 PW 384->384",                    "PW", pw_macs(384, 384, 8, 8))

block_marker("Stage4 blk0 RepMixer C=384 8x8")
repmixer("S4B0", True, 384, 8, 8, 1152, "S4B0")
block_marker("Stage4 blk1 RepMixer C=384 8x8")
repmixer("S4B1", True, 384, 8, 8, 960, "S4B1")  # PRUNED 1152->960

add("FinalDW FPGA DW3 fpg=2 384->768 8->4", "DW", dw_macs(768, 4, 4, 3))
add("SE block C=768 4x4 [ARM]", "ARM_SE", 0)
add("done", "MARK", 0)

# ── Match deltas[] (from the log, in order) 1:1 with rows[] (hand-built,
# same order) -- sanity check the counts agree before trusting the join. ──
if len(deltas) != len(rows):
    print(f"WARNING: log has {len(deltas)} timed entries, table has {len(rows)} rows -- MISMATCH, not joining blindly.")
    for i in range(max(len(deltas), len(rows))):
        d = deltas[i] if i < len(deltas) else None
        r = rows[i] if i < len(rows) else None
        print(i, d, r)
    raise SystemExit(1)

print(f"{len(deltas)} entries matched 1:1. Building table...\n")

total_ms = sum(d[1] for d in deltas)
total_macs = sum(r[2] for r in rows)
total_cycles_theoretical = 0.0
total_cycles_measured = 0.0          # only MAC-bearing (conv/dw/pw) layers
total_cycles_measured_all = 0.0      # every line incl. GELU/Add/SE/markers

print(f"{'label':45s} {'ms':>9s} {'MACs':>12s} {'peak cyc':>10s} {'meas cyc':>10s} {'util%':>7s}")
print("-" * 100)
for (label, ms), (label2, engine, macs, note) in zip(deltas, rows):
    measured_cycles = ms * 1e-3 * CLOCK_HZ
    if engine in PEAK and macs > 0:
        peak_cycles = macs / PEAK[engine]
        util = 100.0 * peak_cycles / measured_cycles if measured_cycles > 0 else 0.0
        total_cycles_theoretical += peak_cycles
        total_cycles_measured += measured_cycles
        print(f"{label[:45]:45s} {ms:9.3f} {macs:12d} {peak_cycles:10.0f} {measured_cycles:10.0f} {util:6.1f}%")
    else:
        # non-MAC lines (GELU/Add/markers/SE) -- still real time, 0 theoretical MAC cycles
        print(f"{label[:45]:45s} {ms:9.3f} {'-':>12s} {'-':>10s} {measured_cycles:10.0f} {'-':>7s}")
    total_cycles_measured_all += measured_cycles

print("-" * 100)
print(f"Total wall time (this run):                 {total_ms:10.2f} ms")
print(f"Total real MACs (FPGA conv/dw/pw layers):   {total_macs:,}")
print(f"Achieved throughput (2*MAC/s):               {2*total_macs/(total_ms*1e-3)/1e9:10.4f} GOPS")
print()
print("=== MAC utilization, two denominators ===")
util_macs_only = 100.0 * total_cycles_theoretical / total_cycles_measured
print(f"(a) vs time spent IN conv/dw/pw engines only:  {util_macs_only:6.2f}%  "
      f"(theoretical {total_cycles_theoretical:,.0f} cyc / measured {total_cycles_measured:,.0f} cyc "
      f"= {total_cycles_measured/CLOCK_HZ*1e3:.1f} ms of the {total_ms:.1f} ms total)")
util_all = 100.0 * total_cycles_theoretical / total_cycles_measured_all
print(f"(b) vs FULL wall-clock time (incl. GELU/Add/SE/dispatch overhead): {util_all:6.2f}%  "
      f"(theoretical {total_cycles_theoretical:,.0f} cyc / {total_cycles_measured_all:,.0f} cyc total)")
print()
print("Peak-engine reference (per ZHR-64): 106 total DSPs x 100MHz x 2 = ~21.2 GOPS peak,")
print(f"achieved {2*total_macs/(total_ms*1e-3)/1e9:.3f} GOPS -> naive peak/achieved ratio "
      f"= {100*(2*total_macs/(total_ms*1e-3)/1e9)/21.2:.2f}% (coarser than (a)/(b) above,")
print("since it assumes all 106 DSPs are usable at once, which they aren't -- only one")
print("engine's DSPs are active at a time under this op_code-dispatch architecture.)")

# ── Per-engine-type summary ───────────────────────────────────────────
print()
print("=== Per-engine-type summary ===")
eng_ms = {"CONV": 0.0, "DW": 0.0, "PW": 0.0, "GELU": 0.0, "ADD": 0.0, "ARM_SE": 0.0, "MARK": 0.0}
eng_mac = {"CONV": 0, "DW": 0, "PW": 0}
for (label, ms), (label2, engine, macs, note) in zip(deltas, rows):
    eng_ms[engine] = eng_ms.get(engine, 0.0) + ms
    if engine in eng_mac:
        eng_mac[engine] += macs
for eng in ["CONV", "DW", "PW", "GELU", "ADD", "ARM_SE", "MARK"]:
    macs = eng_mac.get(eng)
    if macs is not None:
        peak_cyc = macs / PEAK[eng]
        meas_cyc = eng_ms[eng] * 1e-3 * CLOCK_HZ
        u = 100.0 * peak_cyc / meas_cyc if meas_cyc else 0.0
        print(f"  {eng:8s} {eng_ms[eng]:9.2f} ms  ({100*eng_ms[eng]/total_ms:5.1f}% of total)  "
              f"MACs={macs:>12,}  util={u:6.2f}%")
    else:
        print(f"  {eng:8s} {eng_ms[eng]:9.2f} ms  ({100*eng_ms[eng]/total_ms:5.1f}% of total)  (no MACs)")
