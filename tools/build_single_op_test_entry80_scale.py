"""
build_single_op_test_entry80_scale.py -- A3 board bring-up (ZHR-92,
2026-08-23): package entry[80] (SCALE, real SE-block shape, the ONLY
SCALE in the whole 82-entry network) into a compact single-op board test
bundle. SCALE has never been board-tested on this architecture -- csim-
only until now (mac_array_tb.cpp Phase 6, as part of the whole SE flow).

SCALE is the one two-input op in this batch (op0=in_off is the full
feature map, op1=in2_off is the broadcast gate -- same
relocate-both-into-one-in.bin convention already proven working by
entry10_add's bundle). Real op0 is entry[74]'s (final DWCONV) real output
-- the SAME feature map GAP read (g_hw_seq[80].in_off=1769472 ==
g_hw_seq[74].out_off, confirmed from mac_array_ckpt_desc.h, matching
final_conv's real fan_out=2 into both ReduceMean and this Mul). Real op1
is entry[79]'s (SIGMOID) real output (g_hw_seq[80].in2_off=0 ==
g_hw_seq[79].out_off). Reference is entry_80.bin.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_entry80_scale")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

OP_SCALE = 6
cin, cout = 768, 768
h_in, w_in = 8, 8
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 7

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_74.bin"), "rb") as f:
    feat_buf = f.read()
assert len(feat_buf) == cin * h_in * w_in, f"entry_74.bin size {len(feat_buf)} != {cin*h_in*w_in}"

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_79.bin"), "rb") as f:
    gate_buf = f.read()
assert len(gate_buf) == cin, f"entry_79.bin size {len(gate_buf)} != {cin} (gate is cin values)"

with open(os.path.join(ROOT, "accuracy_test_imgs_256", "entry_80.bin"), "rb") as f:
    ref_out = f.read()
assert len(ref_out) == cin * h_in * w_in

# Relocated layout: feature map at 0, gate immediately after.
in_off = 0
in2_off = len(feat_buf)
in_buf = feat_buf + gate_buf

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
    OP_SCALE, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    out_shift,
    in_off, 0, 0, 0,   # in_off=0 (feature map), w_off/b_off unused, out_off=0 (relocated)
    in2_off,           # gate, immediately after the feature map in in.bin
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
print(f">>> in.bin: {len(in_buf)} bytes (feature {len(feat_buf)} + gate {len(gate_buf)}, in2_off={in2_off})")
print(f">>> ref_out.bin: {len(ref_out)} bytes")
print(f">>> bundle written to {OUT_DIR}")
