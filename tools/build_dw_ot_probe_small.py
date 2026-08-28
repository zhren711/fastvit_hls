"""
build_dw_ot_probe_small.py -- A3 DW ot-level fixed-cost probe, n_ot=1 point
(ZHR-92, 2026-08-23). Companion to build_dw_ot_probe_large.py -- same
two-point linear-decomposition method as the PW glue-isolation/decomp
experiments (build_glue_isolation_test.py / build_glue_decomp_test.py),
but applied to DW's own repeating axis. DW has no cbase-style chunking
loop within one ot (one ot = one run_reduce_dw call over the full,
compile-time-fixed MAX_K*MAX_K=49-tap trip count, masked down to the
real K*K taps) -- so unlike PW (which held n_ot large and constant while
varying n_cbase), DW's only available repeating axis IS n_ot itself.
Comparing this n_ot=1 point against build_dw_ot_probe_large.py's n_ot=500
point isolates DW's per-ot REPEATING cost (steady-state, dominant at
large n_ot) from any one-time per-CALL dispatch overhead (AP_START/first-
read pipeline fill/etc, dominant at n_ot=1 since there's nothing to
amortize it against).

Shape: cin=cout=2 (fpg=1, one MAC_PD=2-wide channel tile exactly,
n_ch_tiles=1 -> n_ot=1), h_in=w_in=4, k=1/stride=1/pad=0 (h_out=w_out=4,
one MAC_PR x MAC_PC spatial tile, no boundary/padding edge cases).
K=1 is deliberate: GATHER_ALL_DW/UNIFIED's loop trip count is ALWAYS the
compile-time MAX_K*MAX_K=49 regardless of real K (masked by `valid`), so
K doesn't affect the per-ot hardware cost being measured here -- K=1 is
just the simplest, most cheaply hand-computable choice.

Deterministic all-1s input/weight, zero bias, shift=0: output =
1*1 + 0 = 1 per element (48 of 49 taps are masked to weight=0 and
contribute nothing), hand-computable, no clipping.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_dw_ot_small")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD = 4, 4, 1

OP_DWCONV = 0
cin, cout = 2, 2
h_in, w_in = 4, 4
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 32 bytes, all 1
w_buf = bytes([1]) * (cout * k * k)                 # 2 bytes, all 1 (fpg=1 -> cout=cin*fpg weights, K*K each)
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 8 bytes, all 0

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
assert n_row_tiles == 1 and n_col_tiles == 1 and n_ch_tiles == 1, "n_ot must be 1"

ref_out = bytes([1]) * (cout * h_out * w_out)  # 32 bytes, every element = 1

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
