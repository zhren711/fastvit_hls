"""
build_full_network_test.py -- A3 first full end-to-end board run (ZHR-92,
2026-08-23). Parses mac_array_ckpt_desc.h's g_hw_seq[] literal directly
(the SAME 82 real descriptors already csim-validated byte-exact by
mac_array_ckpt_dump.cpp, 0 errors) rather than regenerating them from the
JSON sources independently -- avoids any chance of a relocation/parsing
mismatch between this script's descriptors and the ones csim already
proved correct (see build_single_op_test_entry5_dw.py's own history: an
earlier version's independently-derived offset was wrong and only caught
by a bundle-verification tool after a board failure).

Computes h_out/w_out/n_row_tiles/n_col_tiles/n_ch_tiles/last_*_tile/
in_ch_stride/out_ch_stride the same way derive_mac_array_params() does
(ported verbatim), packs each entry into MacLayerDesc's exact 27-int
layout (mac_array_driver.h), and writes ONE flat desc_all.bin (82*27*4
bytes) for the board harness to load and dispatch entry-by-entry, using
REAL (not relocated) in_off/out_off/w_off/b_off -- this is a single flat
arena shared across all 82 entries (Route C), not 82 independent bundles.
"""
import re
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
MAC_PR, MAC_PC, MAC_PD = 4, 4, 2
N_HW_SEQ = 82

with open(os.path.join(ROOT, "fastvit_ip_v2", "mac_array_ckpt_desc.h")) as f:
    text = f.read()

# Main literal: { op_type,cin,cout, h_in,w_in, k,stride,pad, fpg, out_shift,
#                 in_off,w_off,b_off,out_off, in2_off },
row_re = re.compile(
    r"\{\s*(-?\d+),\s*(-?\d+),(-?\d+),\s*(-?\d+),(-?\d+),\s*(-?\d+),(-?\d+),(-?\d+),\s*"
    r"(-?\d+),\s*(-?\d+),\s*(-?\d+),(-?\d+),(-?\d+),(-?\d+),\s*(-?\d+)\s*\}"
)
rows = row_re.findall(text)
assert len(rows) == N_HW_SEQ, f"parsed {len(rows)} entries, expected {N_HW_SEQ}"

# use_shift_table / shift_off: g_hw_seq[N].use_shift_table = X; g_hw_seq[N].shift_off = Y;
shift_re = re.compile(
    r"g_hw_seq\[(\d+)\]\.use_shift_table\s*=\s*(-?\d+);\s*"
    r"g_hw_seq\[\1\]\.shift_off\s*=\s*(-?\d+);"
)
shift_fields = {int(n): (int(u), int(s)) for n, u, s in shift_re.findall(text)}
assert len(shift_fields) == N_HW_SEQ, f"parsed {len(shift_fields)} shift-table entries, expected {N_HW_SEQ}"

def derive(cin, h_in, w_in, k, stride, pad):
    h_out = (h_in + 2 * pad - k) // stride + 1
    w_out = (w_in + 2 * pad - k) // stride + 1
    ch_dim = cin
    n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
    n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
    n_ch_tiles = (ch_dim + MAC_PD - 1) // MAC_PD
    last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
    last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
    last_ch_tile = ch_dim - (n_ch_tiles - 1) * MAC_PD
    in_ch_stride = h_in * w_in
    out_ch_stride = h_out * w_out
    return (h_out, w_out, n_row_tiles, n_col_tiles, n_ch_tiles,
            last_row_tile, last_col_tile, last_ch_tile, in_ch_stride, out_ch_stride)

all_desc_bytes = bytearray()
summary = []
for i, r in enumerate(rows):
    (op_type, cin, cout, h_in, w_in, k, stride, pad, fpg, out_shift,
     in_off, w_off, b_off, out_off, in2_off) = map(int, r)
    (h_out, w_out, n_row_tiles, n_col_tiles, n_ch_tiles,
     last_row_tile, last_col_tile, last_ch_tile,
     in_ch_stride, out_ch_stride) = derive(cin, h_in, w_in, k, stride, pad)
    use_shift_table, shift_off = shift_fields[i]

    fields = [
        op_type, cin, cout,
        h_in, w_in,
        k, stride, pad,
        fpg,
        out_shift,
        in_off, w_off, b_off, out_off,
        in2_off,
        h_out, w_out,
        n_row_tiles, n_col_tiles, n_ch_tiles,
        last_row_tile, last_col_tile, last_ch_tile,
        use_shift_table, shift_off,
        in_ch_stride, out_ch_stride,
    ]
    assert len(fields) == 27, len(fields)
    all_desc_bytes += struct.pack("<27i", *fields)
    summary.append((i, op_type, cin, cout, h_in, w_in, in_off, out_off))

assert len(all_desc_bytes) == N_HW_SEQ * 27 * 4

OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_full_network")
os.makedirs(OUT_DIR, exist_ok=True)
with open(os.path.join(OUT_DIR, "desc_all.bin"), "wb") as f:
    f.write(all_desc_bytes)

print(f">>> parsed {N_HW_SEQ} real entries from mac_array_ckpt_desc.h")
print(f">>> desc_all.bin: {len(all_desc_bytes)} bytes ({N_HW_SEQ} x 27 x 4)")
for i, op_type, cin, cout, h_in, w_in, in_off, out_off in summary[:5]:
    print(f"    [{i:2d}] op={op_type} cin={cin} cout={cout} h={h_in} w={w_in} in_off={in_off} out_off={out_off}")
print("    ...")
print(f">>> bundle written to {OUT_DIR}")
