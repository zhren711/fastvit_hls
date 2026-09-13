"""
decompose_full_network_log.py -- ZHR-92 (2026-09-12): per-operator time
decomposition of a mac_array_full_network_test log (the ">>> [ i] op_type=N
... dispatching..." / ">>> [ i] done: X.XXms" line pairs, all 82 entries --
NOT the log's own top-10 summary, which only lists 10). Prints each op
family's total ms and, optionally, the delta against a baseline
decomposition given as name=ms pairs.

    python tools/decompose_full_network_log.py <log> [name=ms ...]
    python tools/decompose_full_network_log.py run1.log DWCONV=531.20 PWCONV=496.08 GELU=22.93 ADD=13.06 SE=12.95
"""
import re
import sys

NAMES = {0: "DWCONV", 1: "PWCONV", 2: "ADD", 3: "GAP", 4: "RELU", 5: "SIGMOID", 6: "SCALE", 7: "GELU"}
FAMILY = {"GAP": "SE", "RELU": "SE", "SIGMOID": "SE", "SCALE": "SE"}


def decompose(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    ops = {int(i): int(op) for i, op in re.findall(r">>> \[\s*(\d+)\] op_type=(\d+) ", txt)}
    done = {int(i): float(ms) for i, ms in re.findall(r">>> \[\s*(\d+)\] done: ([0-9.]+)ms", txt)}
    acc = {}
    for i, ms in done.items():
        fam = NAMES[ops[i]]
        fam = FAMILY.get(fam, fam)
        acc[fam] = acc.get(fam, 0.0) + ms
    total = re.findall(r"PL-side total.*?: ([0-9.]+) ms", txt)
    return acc, len(done), (float(total[-1]) if total else None)


if __name__ == "__main__":
    acc, n, total = decompose(sys.argv[1])
    base = dict(kv.split("=") for kv in sys.argv[2:])
    print(f"entries: {n}   PL-side total: {total} ms   sum of per-entry: {sum(acc.values()):.2f} ms")
    for fam in ("DWCONV", "PWCONV", "GELU", "ADD", "SE"):
        v = acc.get(fam, 0.0)
        line = f"  {fam:8s} {v:8.2f} ms  ({100.0 * v / total:5.2f}%)"
        if fam in base:
            b = float(base[fam])
            line += f"   baseline {b:8.2f}  delta {v - b:+7.2f} ms ({100.0 * (v - b) / b:+.2f}%)"
        print(line)
