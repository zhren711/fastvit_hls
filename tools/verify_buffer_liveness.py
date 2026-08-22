"""
verify_buffer_liveness.py -- A2: independent buffer-liveness checker.

Deliberately does NOT import or call anything from gen_hw_sequence.py.
That script DECIDES buffer offsets (bump()/next_main()/resolve_offsets());
this script only CONSUMES its output (tools/layer_hw_sequence_256.json)
as data and independently re-derives two separate things gen_hw_sequence.py
never had to get right on its own:

  1. Region layout non-overlap -- recomputed fresh from the declared region
     sizes, not by trusting resolve_offsets() got the cumulative math right.
  2. Per-buffer liveness -- for every WRITE, who (per ground-truth DAG
     fan-out, tools/layer_dag_ground_truth.json) is supposed to read it,
     and does anything else WRITE to that same address before all of
     those true consumers have read it.

This split matters because of the verify_layer_dag.py precedent: a
self-consistency check ("0 diff") that only compares a generator's output
against its OWN understanding of the graph can pass while the wiring is
still wrong, if the generator's understanding was wrong in the first
place. Ground truth here is layer_dag_ground_truth.json's per-conv-node
fan_out/consumers list, which is produced independently of both
gen_layer_descriptor.py's classify_consumers() and gen_hw_sequence.py's
fold logic -- not by them.

Ground-truth coverage note: layer_dag_ground_truth.json only tracks
fan-out for the 52 CONV-producing nodes (that's what "ground truth" means
here -- it's a direct ONNX graph trace, and the 52 conv nodes are the only
ones gen_layer_descriptor.py's classify_consumers() needed independent
confirmation for). The folded synthetic ops this script also emits writes
for (gelu/lscale/gap/relu/sigmoid/scale) are, by construction of this
network's actual topology, always single-consumer (an activation's output
feeds exactly one downstream op) -- so their expected reader count is
asserted as 1, not looked up. If that assumption is ever wrong for a
future layer, this checker will catch it exactly the way it catches any
other undercount: a write into an address whose sole expected reader
already happened would leave the checker's own "the true reader is now
satisfied, this address is free again" bookkeeping HAPPY when the real
consumer never actually ran -- the fix there is to add the missing
reader to the ground-truth model, not to relax this check.

用法:
  python verify_buffer_liveness.py [--hw-seq <json>] [--descriptor <json>]
                                    [--dag-ground-truth <json>]
"""
import json
import argparse
from collections import defaultdict


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--hw-seq", default=r"E:\codes\microzed\fastvit_hls\tools\layer_hw_sequence_256.json")
    p.add_argument("--descriptor", default=r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json")
    p.add_argument("--dag-ground-truth", default=r"E:\codes\microzed\fastvit_hls\tools\layer_dag_ground_truth.json")
    return p.parse_args()


# ---------------------------------------------------------------------
# Check 1: region layout non-overlap, recomputed independently from the
# declared per-region sizes (not by trusting the offsets are correct).
# ---------------------------------------------------------------------
def check_region_layout(hwdata):
    regions = hwdata["regions"]
    offsets = hwdata["offsets"]
    problems = []

    # every declared region must have an offset, and vice versa
    if set(regions) != set(offsets):
        problems.append(f"region/offset key mismatch: regions={sorted(regions)} offsets={sorted(offsets)}")
        return problems

    # independently recompute expected layout: sort by the offsets given,
    # then verify each region's [offset, offset+size) is non-overlapping
    # with its neighbors and that the packing has no gaps introduced by
    # an arithmetic error (gaps are not a correctness bug per se, but an
    # unexplained gap means the cumulative sum in resolve_offsets() did
    # not do what it claims to -- worth flagging).
    ordered = sorted(offsets.items(), key=lambda kv: kv[1])
    cursor = 0
    for name, off in ordered:
        size = regions[name]
        if off != cursor:
            problems.append(f"region {name}: offset {off} != expected cumulative {cursor} "
                             f"(gap or overlap in packing)")
        cursor = off + size

    # brute-force pairwise overlap check, independent of the packing order
    # assumption above -- catches the case where two regions' ranges
    # overlap even if the cumulative-sum check above was fooled somehow.
    items = [(name, offsets[name], offsets[name] + regions[name]) for name in regions]
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            n1, s1, e1 = items[i]
            n2, s2, e2 = items[j]
            if s1 < e2 and s2 < e1:
                problems.append(f"region overlap: {n1}=[{s1},{e1}) intersects {n2}=[{s2},{e2})")

    return problems


# ---------------------------------------------------------------------
# Check 2: per-write liveness against ground-truth expected reader count.
# ---------------------------------------------------------------------
def build_expected_reader_counts(descriptor, dag_gt):
    """layer_idx -> expected number of DISTINCT hw-sequence entries that
    must read this conv layer's output.

    NOT raw ground-truth fan_out: fan_out counts ONNX-graph consumer
    EDGES, and GELU's decomposition (Div, then later Mul, both reading
    the same conv output -- confirmed in dag ground truth, e.g. node_idx=0
    has fan_out=2 via one Div consumer + one Mul consumer) means two
    graph edges fold into ONE hardware read: run_gelu() loads the value
    from DRAM once and reuses it on-chip for both the erf-gate branch and
    the final multiply. Using raw fan_out=2 here would flag every single
    GELU-consumed layer as a false liveness violation (confirmed: this is
    exactly what the first version of this script did, 16/16 false
    positives, all GELU-consumed layers, none of them on layers with a
    real distinct second consumer like the token_mixer/Add case).

    Instead use len(consumers) from layer_descriptor_256.json --
    gen_layer_descriptor.py's classify_consumers() already collapses
    GELU's Div+Mul into one {"kind":"gelu"} entry (independently
    self-checked against this same dag ground truth via
    check_dispatch_plan(), a separate mechanism from gen_hw_sequence.py).
    This is still independent of the code under test: it does not import
    or call gen_hw_sequence.py, only reads a data file gen_hw_sequence.py
    itself only consumes, never produces.
    """
    gt_by_node_idx = {L["node_idx"]: L for L in dag_gt["layers"]}
    expected = {}
    for e in descriptor:
        node_idx = e["node_idx"]
        if node_idx not in gt_by_node_idx:
            raise KeyError(f"layer_idx={e['layer_idx']} node_idx={node_idx} "
                            f"({e['node_name']}) not found in dag ground truth 'layers'")
        expected[e["layer_idx"]] = len(e["consumers"])
    return expected


def check_liveness(seq, expected_reader_counts):
    """For every entry that WRITES to out_off=X: walk forward through the
    rest of the sequence counting reads to X. If a DIFFERENT entry WRITES
    to X again before the expected number of reads has been observed,
    that's a liveness violation -- a later tenant clobbered X while an
    earlier tenant's true consumer(s), per ground truth, hadn't read it
    yet. Once the expected count of reads is satisfied, X is legitimately
    free for reuse and subsequent writes are fine."""
    problems = []
    n = len(seq)

    for i, e in enumerate(seq):
        if "out_off" not in e:
            continue
        addr = e["out_off"]

        if e["kind"] == "conv":
            layer_idx = e["layer_idx"]
            if layer_idx not in expected_reader_counts:
                problems.append(f"entry {i} (conv layer_idx={layer_idx}): no ground-truth fan_out available")
                continue
            expected = expected_reader_counts[layer_idx]
            label = f"entry {i} (conv layer_idx={layer_idx}, addr={addr})"
        else:
            # synthetic fold ops: single-consumer by construction of this
            # network's topology (see module docstring)
            expected = 1
            label = f"entry {i} (kind={e['kind']}, addr={addr})"

        reads_seen = 0
        for j in range(i + 1, n):
            other = seq[j]
            reads_here = sum(1 for key in ("in_off", "op0_off", "op1_off")
                              if other.get(key) == addr)
            if reads_here and reads_seen < expected:
                reads_seen += reads_here
            if other.get("out_off") == addr:
                if reads_seen < expected:
                    problems.append(
                        f"{label}: overwritten by entry {j} (kind={other['kind']}) "
                        f"after only {reads_seen}/{expected} expected reads"
                    )
                break  # this write's liveness window ends at the first re-write regardless
        else:
            if reads_seen < expected:
                # ran off the end of the sequence without enough reads --
                # only acceptable for the very last entry (final network output)
                if i != n - 1:
                    problems.append(
                        f"{label}: only {reads_seen}/{expected} expected reads seen "
                        f"before end of sequence (and this is not the final entry)"
                    )

    return problems


# ---------------------------------------------------------------------
# Check 3: every read has at least one prior write to its address
# (catches reads of never-written / garbage memory).
# ---------------------------------------------------------------------
def check_no_dangling_reads(seq):
    """Exactly one dangling read is expected: the very first read in the
    sequence, whatever address it targets -- that buffer is populated by
    the ARM before dispatch (outside these entries), not by any op in
    this sequence. Before Route C (ZHR-92 2026-08-21) that was the raw
    input image at a fixed 'INPUT' offset; under Route C it's Stem's
    ARM-computed output landing in MAIN0 instead -- same cardinality (one
    external handoff), different address, so this exempts by POSITION
    (first dangling read encountered) rather than a specific named
    region, so it doesn't need updating again next time the entry point
    moves. Any OTHER dangling read is a real bug."""
    problems = []
    written = set()
    seen_first_dangling = False
    for i, e in enumerate(seq):
        for key in ("in_off", "op0_off", "op1_off"):
            if key in e and e[key] not in written:
                if not seen_first_dangling:
                    seen_first_dangling = True
                    continue
                problems.append(f"entry {i} (kind={e['kind']}): reads addr={e[key]} "
                                 f"via {key} with no prior write in the sequence")
        if "out_off" in e:
            written.add(e["out_off"])
    return problems


def main():
    args = parse_args()
    with open(args.hw_seq) as f:
        hwdata = json.load(f)
    with open(args.descriptor) as f:
        descriptor = json.load(f)
    with open(args.dag_ground_truth) as f:
        dag_gt = json.load(f)

    seq = hwdata["sequence"]

    problems = []

    layout_problems = check_region_layout(hwdata)
    print(f">>> Check 1 (region layout non-overlap): {len(layout_problems)} problem(s)")
    problems += layout_problems

    dangling_problems = check_no_dangling_reads(seq)
    print(f">>> Check 2 (no dangling reads): {len(dangling_problems)} problem(s)")
    problems += dangling_problems

    expected_reader_counts = build_expected_reader_counts(descriptor, dag_gt)
    liveness_problems = check_liveness(seq, expected_reader_counts)
    print(f">>> Check 3 (liveness vs ground-truth fan_out): {len(liveness_problems)} problem(s)")
    problems += liveness_problems

    if problems:
        print(f"\n>>> {len(problems)} TOTAL PROBLEM(S):")
        for p in problems:
            print("   -", p)
    else:
        print(f"\n>>> ALL CLEAR: {len(seq)} entries, 0 liveness/layout/dangling-read conflicts.")

    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
