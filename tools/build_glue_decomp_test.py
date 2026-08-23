"""
build_glue_decomp_test.py -- A3 glue decomposition experiment (ZHR-92,
2026-08-23): measures the per-cbase marginal cost vs. the per-ot fixed
cost by comparing this shape (n_cbase=36, the real network's max) against
board_test_glue_isolation's existing n_cbase=1 measurement (870
cycles/ot, already on record).

Shape: cin=1152 (real network max, MAX_CIN -> n_cbase=ceil(1152/32)=36),
cout=500, h=w=4, k=1. n_row_tiles=n_col_tiles=1 (same as the n_cbase=1
experiment -- only cin changed, isolating n_cbase as the sole varying
factor). n_ot=cout=500, so total = 500 x 36 = 18,000 real tile
invocations in one hardware dispatch.

Deterministic all-1s data: each output = sum_{ci=0..1151}(1*1) = 1152,
clipped (int8, shift=0) to 127 -- still a cheap, hand-computable
correctness check (every output byte must be exactly 127).
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_glue_decomp")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD, MAX_CIN_PW = 4, 4, 2, 32

OP_PWCONV = 1
cin, cout = 1152, 500
h_in, w_in = 4, 4
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 18,432 bytes, all 1
w_buf = bytes([1]) * (cout * cin * k * k)           # 576,000 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 2,000 bytes, all 0

h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
assert h_out == 4 and w_out == 4
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out
n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
assert n_row_tiles == 1 and n_col_tiles == 1 and n_cbase == 36 and cout == 500

ref_out = bytes([127]) * (cout * h_out * w_out)  # 8,000 bytes, saturated

fields = [
    OP_PWCONV, cin, cout,
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
    f.write(w_buf)
with open(os.path.join(OUT_DIR, "b.bin"), "wb") as f:
    f.write(b_buf)
with open(os.path.join(OUT_DIR, "ref_out.bin"), "wb") as f:
    f.write(ref_out)

print(f">>> desc.bin: {len(desc_bytes)} bytes")
print(f">>> in.bin: {len(in_buf)} bytes, w.bin: {len(w_buf)} bytes, b.bin: {len(b_buf)} bytes")
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 127, saturated, hand-computed)")
print(f">>> n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_ot={cout} n_cbase={n_cbase}")
print(f">>> total (ot,cbase) tile invocations = {n_row_tiles*n_col_tiles*cout*n_cbase}")
print(f">>> bundle written to {OUT_DIR}")
