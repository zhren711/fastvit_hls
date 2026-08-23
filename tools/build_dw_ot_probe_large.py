"""
build_dw_ot_probe_large.py -- A3 DW ot-level fixed-cost probe, n_ot=4000
point (ZHR-92, 2026-08-23). See build_dw_ot_probe_small.py's header
comment for the full method -- this is the same shape at cin=cout=8000
instead of 2, giving n_ch_tiles=4000 (n_ot=4000) with no partial last
channel tile (8000 is even, last_ch_tile=2 same as every other tile).
Comparing this point's cycles/ot against the n_ot=1 point's isolates
DW's per-ot REPEATING (steady-state) cost from one-time per-CALL
dispatch overhead.

Sized up from an initial n_ot=500 attempt (2026-08-23): that point
measured 6.49ms total against a poll_count of only 7 (~1.08ms/poll),
~17% relative uncertainty -- too coarse to resolve a ~2x qualitative
question cleanly. n_ot=4000 targets a ~50ms total, ~2% relative
uncertainty, same technique the earlier PW glue-decomp round used to
avoid poll-granularity noise on a real question.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_dw_ot_large")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

OP_DWCONV = 0
cin, cout = 8000, 8000
h_in, w_in = 4, 4
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 16,000 bytes, all 1
w_buf = bytes([1]) * (cout * k * k)                 # 1,000 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 4,000 bytes, all 0

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
assert n_row_tiles == 1 and n_col_tiles == 1 and n_ch_tiles == 4000, "n_ot must be 4000"
assert last_ch_tile == MAC_PD, "cin must be an even multiple of MAC_PD -- no partial last tile"

ref_out = bytes([1]) * (cout * h_out * w_out)  # 16,000 bytes, every element = 1

fields = [
    OP_DWCONV, cin, cout,
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
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 1, hand-computed)")
print(f">>> n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_ot={n_ch_tiles}")
print(f">>> bundle written to {OUT_DIR}")
