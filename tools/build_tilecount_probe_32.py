"""
build_tilecount_probe_32.py -- A3 (rt,colt)-level fixed-cost probe, 32x32
point (ZHR-92, 2026-08-24). See build_tilecount_probe_64.py's header
comment for the full method -- this is the same shape (cin=cout=48,
n_cbase=2, n_ot=48) at h_in=w_in=32 instead of 64, giving 64 spatial
tiles instead of 256 (a clean 4x reduction in tile count, everything
else held constant).
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_tilecount_32")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD, MAX_CIN_PW = 4, 4, 2, 32

OP_PWCONV = 1
cin, cout = 48, 48
h_in, w_in = 32, 32
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 49,152 bytes, all 1
w_buf = bytes([1]) * (cout * cin * k * k)           # 2,304 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 192 bytes, all 0

h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
assert h_out == 32 and w_out == 32
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out
n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
assert n_row_tiles == 8 and n_col_tiles == 8 and n_cbase == 2 and cout == 48
print(f">>> total tiles = {n_row_tiles * n_col_tiles} (expect 64)")

ref_out = bytes([48]) * (cout * h_out * w_out)  # every byte = 48, hand-computed

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
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 48, hand-computed)")
print(f">>> n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_ot={cout} n_cbase={n_cbase}")
print(f">>> bundle written to {OUT_DIR}")
