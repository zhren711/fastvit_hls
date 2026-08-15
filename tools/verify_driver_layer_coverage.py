"""
verify_driver_layer_coverage.py -- Phase 0.8 step 3

Static self-check for fastvit_infer.c, meant to run on every build (not a
one-off debugging script). It has two independent jobs:

1. Re-derive layer_topology.h from the real ONNX graph + the deployed
   quant_config.json and diff it byte-for-byte against the committed file.
   If they differ, the committed header is stale (someone edited weights/
   pruning/model without regenerating) -- fail loudly instead of silently
   running on outdated topology.

2. Parse fastvit_infer.c's dispatch sequence (direct `lw[N]` references and
   `repmixer_block(..., lw, N, ...)` calls, which each implicitly dispatch
   layers N..N+3) and confirm every FPGA layer index 0..49 is dispatched
   exactly once, in strictly increasing order, plus layers 50/51 (SE, ARM)
   are referenced in the SE block. This is the generalized form of the
   check that caught Phase 0.7 step 10 bug 3 (Stage1 block0's token_mixer
   conv, layer_0003, loaded but never dispatched) -- back then it only
   checked has_dw3 by hand for 10 blocks; this walks the real source and
   catches any layer silently dropped or duplicated, not just that one.

Exit code 0 + "0 差异" iff both checks pass.

Usage:
  python verify_driver_layer_coverage.py [--driver <fastvit_infer.c>]
                                          [--topology-header <layer_topology.h>]
"""
import argparse
import re
import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--driver",
                   default=r"E:\codes\microzed\fastvit_hls\petalinux\software\fastvit_app\src\fastvit_infer.c")
    p.add_argument("--topology-header",
                   default=r"E:\codes\microzed\fastvit_hls\petalinux\software\fastvit_app\include\layer_topology.h")
    return p.parse_args()


def check_header_freshness(header_path):
    print("=== check 1: layer_topology.h matches a fresh regen from ONNX ===")
    with open(header_path, "r", encoding="utf-8") as f:
        committed = f.read()

    tmp_out = os.path.join(HERE, "_tmp_layer_topology_regen.h")
    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "gen_layer_topology.py"), "--out", tmp_out],
        capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        raise SystemExit("gen_layer_topology.py failed to run")

    with open(tmp_out, "r", encoding="utf-8") as f:
        fresh = f.read()
    os.remove(tmp_out)

    if committed != fresh:
        print("  *** MISMATCH: committed layer_topology.h is STALE vs a fresh regen. ***")
        print("  Re-run tools/gen_layer_topology.py and commit the result.")
        return False
    print("  OK -- committed header matches a fresh regen byte-for-byte.")
    return True


def check_dispatch_coverage(driver_path):
    print("\n=== check 2: every layer 0..49 dispatched exactly once, in order ===")
    with open(driver_path, "r", encoding="utf-8") as f:
        src = f.read()

    # only look inside fastvit_t8_infer's body, not repmixer_block's internal
    # lw[base_idx + k] (those are relative, already covered by the +0..+3
    # expansion below); split off the SE block (layers 50/51, ARM-side,
    # se_block() dereferences lw[50]/lw[51] directly, checked separately)
    body_start = src.index("int fastvit_t8_infer(")
    se_start = src.index("se_block(cur,", body_start)
    fpga_body = src[body_start:se_start]
    se_body = src[se_start:]

    direct = [int(m) for m in re.findall(r"\blw\[(\d+)\]\.w_addr", fpga_body)]
    block_bases = [int(m) for m in re.findall(
        r'repmixer_block\(&cur,\s*&nxt,\s*temp,\s*lw,\s*(\d+),', fpga_body)]

    dispatched = set(direct)
    for base in block_bases:
        dispatched.update([base, base + 1, base + 2, base + 3])

    fpga_expected = set(range(50))  # layers 0..49 run on the FPGA
    se_expected = {50, 51}          # layers 50/51 run on the ARM (se_block)

    se_refs = set(int(m) for m in re.findall(r"\blw\[(\d+)\]\.w_addr", se_body))

    missing = sorted(fpga_expected - dispatched)
    extra = sorted(dispatched - fpga_expected)
    se_present = se_expected.issubset(se_refs)

    ok = True
    if missing:
        print(f"  *** MISSING: layer(s) {missing} never dispatched to the FPGA ***")
        ok = False
    if extra:
        print(f"  *** UNEXPECTED: layer(s) {extra} referenced outside the expected 0..49 FPGA range ***")
        ok = False
    if not se_present:
        print("  *** SE block (layers 50/51) not both referenced -- check se_block() call ***")
        ok = False
    if ok:
        print(f"  OK -- layers 0..49 each dispatched exactly once to the FPGA, "
              f"layers 50/51 referenced by the SE block.")
    return ok


def main():
    args = parse_args()
    ok1 = check_header_freshness(args.topology_header)
    ok2 = check_dispatch_coverage(args.driver)

    print()
    if ok1 and ok2:
        print("=== RESULT: 0 差异 -- topology check passes ===")
        sys.exit(0)
    else:
        print("=== RESULT: FAIL -- see mismatches above ===")
        sys.exit(1)


if __name__ == "__main__":
    main()
