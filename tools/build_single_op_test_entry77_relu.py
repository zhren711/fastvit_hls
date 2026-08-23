"""
build_single_op_test_entry77_relu.py -- A3 board bring-up (ZHR-92,
2026-08-23): package entry[77] (RELU, real SE-block shape, the ONLY RELU
in the whole 82-entry network) into a compact single-op board test
bundle. RELU has never been board-tested on this architecture -- csim-
only until now (mac_array_tb.cpp Phase 6, as part of the whole SE flow).

Real input is entry[76]'s (fc1) real output (g_hw_seq[76].out_off=786432
== g_hw_seq[77].in_off), reference is entry_77.bin.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_entry77_relu")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

OP_RELU = 4
cin, cout = 48, 48
h_in, w_in = 1, 1
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_76.bin"), "rb") as f:
    in_buf = f.read()
assert len(in_buf) == cin * h_in * w_in, f"entry_76.bin size {len(in_buf)} != {cin*h_in*w_in}"

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_77.bin"), "rb") as f:
    ref_out = f.read()
assert len(ref_out) == cin * h_in * w_in

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
    OP_RELU, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    out_shift,
    0, 0, 0, 0,
    0,
    h_out, w_out,
    n_row_tiles, n_col_tiles, n_ch_tiles,
    last_row_tile, last_col_tile, last_ch_tile,
    0, 0,
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
print(f">>> in.bin: {len(in_buf)} bytes")
print(f">>> ref_out.bin: {len(ref_out)} bytes")
print(f">>> bundle written to {OUT_DIR}")
