"""
build_single_op_test_entry75_gap.py -- A3 board bring-up (ZHR-92,
2026-08-23): package entry[75] (GAP, real SE-block shape, the ONLY GAP in
the whole 82-entry network) into a compact single-op board test bundle.
GAP has never been board-tested on this architecture -- csim-only until
now (mac_array_tb.cpp Phase 6, as part of the whole SE flow).

Real input is entry[74]'s real output (the final DWCONV's feature map --
g_hw_seq[74].out_off=1769472 == g_hw_seq[75].in_off, confirmed from
mac_array_ckpt_desc.h, not assumed), reference is entry_75.bin (this
project's own real-hardware-via-csim output for entry 75).

No weight/bias needed (GAP is a pure reduction op) -- w.bin/b.bin are
written as empty files, same convention as the GELU bundle.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_entry75_gap")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

# entry[75] real fields, verbatim from mac_array_ckpt_desc.h's g_hw_seq[75].
OP_GAP = 3
cin, cout = 768, 768
h_in, w_in = 8, 8
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_74.bin"), "rb") as f:
    in_buf = f.read()
assert len(in_buf) == cin * h_in * w_in, f"entry_74.bin size {len(in_buf)} != {cin*h_in*w_in}"

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_75.bin"), "rb") as f:
    ref_out = f.read()
assert len(ref_out) == cin, f"entry_75.bin size {len(ref_out)} != {cin} (GAP output is cin values)"

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

fields = [
    OP_GAP, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    out_shift,
    0, 0, 0, 0,  # in_off, w_off, b_off, out_off -- all relocated to 0
    0,          # in2_off (unused)
    h_out, w_out,
    n_row_tiles, n_col_tiles, n_ch_tiles,
    last_row_tile, last_col_tile, last_ch_tile,
    0,          # use_shift_table
    0,          # shift_off
    in_ch_stride, out_ch_stride,
]
assert len(fields) == 27, len(fields)
desc_bytes = struct.pack("<27i", *fields)

with open(os.path.join(OUT_DIR, "desc.bin"), "wb") as f:
    f.write(desc_bytes)
with open(os.path.join(OUT_DIR, "in.bin"), "wb") as f:
    f.write(in_buf)
with open(os.path.join(OUT_DIR, "w.bin"), "wb") as f:
    pass
with open(os.path.join(OUT_DIR, "b.bin"), "wb") as f:
    pass
with open(os.path.join(OUT_DIR, "ref_out.bin"), "wb") as f:
    f.write(ref_out)

print(f">>> desc.bin: {len(desc_bytes)} bytes")
print(f">>> in.bin: {len(in_buf)} bytes (cin={cin} h={h_in} w={w_in})")
print(f">>> w.bin/b.bin: empty (GAP needs no weight/bias)")
print(f">>> ref_out.bin: {len(ref_out)} bytes (cin={cin} channel averages)")
print(f">>> bundle written to {OUT_DIR}")
