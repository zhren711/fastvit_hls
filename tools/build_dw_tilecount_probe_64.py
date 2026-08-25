"""
build_dw_tilecount_probe_64.py -- ZHR-92 (2026-08-25): DW per-tile cost
decomposition, 64-tile point. Companion to entry5_dw's own real bundle
(cin=cout=48, h=w=64, K=3/S=1 -- 256 tiles), which already stands in as
this experiment's 256-tile point (byte-exact, ~70.0-70.2ms across many
repeated board measurements this session).

Same n_ot (=n_ch_tiles=24, since cin=48) as entry5_dw, same K=3/S=1/pad=1
("same" padding, h_out=h_in), but h_in=w_in=32 -> 64 tiles instead of
256 -- a clean 4x tile-count change at fixed n_ot, mirroring the
established (rt,colt) tile-count-scaling probe's own methodology (PW's
own 256-vs-64-tile round, ZHR-92 commit b2a03c7).

Combined with entry5_dw's real board time, isolates DW's per-TILE cost,
complementing the already-valid per-OT cost (1,187.5 cycles/ot, commit
c8400b1 -- confirmed via commit ordering to predate the DW flattening
attempt, and DW's source today is byte-identical to what c8400b1
measured, so that number still stands without re-measurement).

Deterministic all-1s input/weight, zero bias, shift=0 (K=3, 9 real taps,
all weight=1): output = sum_{kh,kw}(1*1) = 9 per element (no clipping,
9 < 127).
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_dw_tilecount_64")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 2

OP_DWCONV = 0
cin, cout = 48, 48
h_in, w_in = 32, 32
k, stride, pad, fpg = 3, 1, 1, 1

W_SLICE_BYTES = cout * k * k          # 48*9 = 432
B_SLICE_ELEMS = cout                   # 48

in_buf = bytes([1]) * (cin * h_in * w_in)
w_buf = bytes([1]) * W_SLICE_BYTES
b_buf = struct.pack("<%di" % B_SLICE_ELEMS, *([0] * B_SLICE_ELEMS))

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

assert h_out == 32 and w_out == 32
assert n_row_tiles == 8 and n_col_tiles == 8, (n_row_tiles, n_col_tiles)
assert n_ch_tiles == 24, n_ch_tiles  # same n_ot as entry5_dw

ref_out = bytes([9]) * (cout * h_out * w_out)

fields = [
    OP_DWCONV, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    0,
    0, 0, 0, 0,
    0,
    h_out, w_out,
    n_row_tiles, n_col_tiles, n_ch_tiles,
    last_row_tile, last_col_tile, last_ch_tile,
    0,          # use_shift_table=0, scalar out_shift=0
    0,
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
print(f">>> in.bin: {len(in_buf)} bytes (cin={cin} h={h_in} w={w_in})")
print(f">>> w.bin: {len(w_buf)} bytes, b.bin: {len(b_buf)} bytes")
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 9, hand-computed)")
print(f">>> total tiles = {n_row_tiles*n_col_tiles} (expect 64, vs entry5_dw's 256), n_ch_tiles={n_ch_tiles} (same as entry5_dw)")
print(f">>> bundle written to {OUT_DIR}")
