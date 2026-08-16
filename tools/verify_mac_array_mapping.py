"""
verify_mac_array_mapping.py -- Phase A DW+PW PoC, independent cross-check
of fastvit_ip_v2/mac_array.cpp's derive_mac_array_params() (ZHR-91 row 6,
requirement 2: "descriptor 字段到 MAC 阵列参数的映射有独立校验").

This is NOT a wrapper around the C++ code and does NOT import or parse
mac_array.cpp -- it reimplements the same arithmetic from scratch, in a
different language, from the same plain description of the contract (see
mac_array.h's MacArrayParams docstring). The point is that a bug in the
tiling arithmetic has to be present in TWO independently-authored
implementations to go undetected, not one.

Reads mac_array_params_dump.txt (written by mac_array_tb.cpp's Phase 1,
one line per layer with both the descriptor fields and the C++ module's
derived params) and recomputes n_row_tiles/n_col_tiles/n_ch_tiles/
last_*_tile from the descriptor fields alone, then diffs against what the
dump says the C++ side produced.

用法:
  python verify_mac_array_mapping.py [--dump fastvit_ip_v2/mac_array_params_dump.txt]
"""
import argparse
import os
import re
import sys

MAC_PR = MAC_PC = MAC_PD = 8
OP_DWCONV, OP_PWCONV = 0, 1


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--dump", default=os.path.join(
        os.path.dirname(__file__), "..", "fastvit_ip_v2", "mac_array_params_dump.txt"))
    return p.parse_args()


def parse_line(line):
    fields = dict(re.findall(r"(\w+)=(-?\d+)", line))
    return {k: int(v) for k, v in fields.items()}


def independent_derive(d):
    """Same contract as mac_array.h's MacArrayParams, computed from scratch."""
    h_out = (d["h_in"] + 2 * d["pad"] - d["k"]) // d["stride"] + 1
    w_out = (d["w_in"] + 2 * d["pad"] - d["k"]) // d["stride"] + 1
    ch_dim = d["cin"] if d["op_type"] == OP_DWCONV else d["cout"]

    n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
    n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
    n_ch_tiles = (ch_dim + MAC_PD - 1) // MAC_PD

    last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
    last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
    last_ch_tile = ch_dim - (n_ch_tiles - 1) * MAC_PD

    return dict(h_out=h_out, w_out=w_out, n_row_tiles=n_row_tiles, n_col_tiles=n_col_tiles,
                n_ch_tiles=n_ch_tiles, last_row_tile=last_row_tile, last_col_tile=last_col_tile,
                last_ch_tile=last_ch_tile)


def main():
    args = parse_args()
    if not os.path.exists(args.dump):
        print(f">>> ERROR: {args.dump} not found -- run the mac_array_tb csim first "
              f"(fastvit_ip_v2/run_csim.tcl) to produce it.")
        sys.exit(2)

    total_mismatches = 0
    with open(args.dump) as f:
        lines = [l for l in f if l.strip()]

    print(f">>> checking {len(lines)} layer(s) against {args.dump}")
    for line in lines:
        d = parse_line(line)
        expected = independent_derive(d)
        actual = {k: d[k] for k in expected}

        mismatches = {k: (expected[k], actual[k]) for k in expected if expected[k] != actual[k]}
        status = "OK" if not mismatches else "MISMATCH"
        print(f"  layer={d['layer']} op_type={d['op_type']} "
              f"cin={d['cin']} cout={d['cout']} h_in={d['h_in']} w_in={d['w_in']} "
              f"k={d['k']} stride={d['stride']} pad={d['pad']}  -> {status}")
        if mismatches:
            total_mismatches += len(mismatches)
            for k, (exp, act) in mismatches.items():
                print(f"      {k}: independent={exp}  mac_array.cpp={act}")

    print()
    if total_mismatches == 0:
        print(">>> PASS: descriptor-to-MAC-array-parameter mapping matches the independent "
              "re-derivation for all layers.")
        sys.exit(0)
    else:
        print(f">>> FAIL: {total_mismatches} field mismatch(es) -- mac_array.cpp's "
              f"derive_mac_array_params() has drifted from the documented contract.")
        sys.exit(1)


if __name__ == "__main__":
    main()
