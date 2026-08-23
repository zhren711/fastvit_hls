"""
build_glue_isolation_test.py -- A3 glue-logic isolation experiment (ZHR-92,
2026-08-23): a synthetic PW descriptor designed to isolate run_layer's
per-(ot,cbase) glue cost from real hardware, not infer it from csynth
region reports (which can't see it -- confirmed, no report covers
run_layer's own sequential body).

Shape: cin=32, cout=1000, h=w=4, k=1, stride=1, pad=0, fpg=1.
  n_row_tiles = n_col_tiles = 1  (h_out=w_out=4, one MAC_PR x MAC_PC tile)
  n_cbase     = 1                (cin=32 fits exactly one MAX_CIN_PW chunk)
  n_ot        = cout = 1000
  => exactly 1,000 real (ot,cbase) tile invocations in ONE hardware
     dispatch -- the entire measured time is 1000x the per-(ot,cbase) unit
     cost (FIXED_PER_OT + 1*PER_CBASE_UNIT from the ot-batching model),
     with accumulation happening INSIDE hardware so ARM-side poll
     granularity (500us-1ms) can't swamp a single tiny dispatch's timing
     the way repeating 1000 SEPARATE dispatches would.

Deterministic data (all-1s input and weights, zero bias/shift) makes the
expected output value trivially hand-computable (sum over cin=32 of
1*1 = 32, no clipping) for a cheap correctness spot-check alongside the
timing measurement -- not a rigorous golden-reference build, since PWCONV
math itself is already exhaustively verified elsewhere; this shape's only
untested aspect is running at cout=1000 (well under MAX_PW_BIAS_CACHE=1152)
without hanging or corrupting anything at that ot count.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_glue_isolation")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD, MAX_CIN_PW = 4, 4, 2, 32

OP_PWCONV = 1
cin, cout = 32, 1000
h_in, w_in = 4, 4
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)          # 512 bytes, all 1
w_buf = bytes([1]) * (cout * cin * k * k)           # 32,000 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))   # 4,000 bytes, all 0

h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
assert h_out == 4 and w_out == 4
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD  # unused for PW, harmless
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out
n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
assert n_row_tiles == 1 and n_col_tiles == 1 and n_cbase == 1 and cout == 1000

# Expected output: every element = sum_{ci=0..31}(1*1) = 32, no clip needed.
ref_out = bytes([32]) * (cout * h_out * w_out)  # 16,000 bytes

fields = [
    OP_PWCONV, cin, cout,
    h_in, w_in,
    k, stride, pad,
    fpg,
    out_shift,
    0, 0, 0, 0,  # in_off, w_off, b_off, out_off -- all relocated to 0
    0,           # in2_off (unused)
    h_out, w_out,
    n_row_tiles, n_col_tiles, n_ch_tiles,
    last_row_tile, last_col_tile, last_ch_tile,
    0,           # use_shift_table
    0,           # shift_off
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
print(f">>> ref_out.bin: {len(ref_out)} bytes (every byte = 32, hand-computed)")
print(f">>> n_row_tiles={n_row_tiles} n_col_tiles={n_col_tiles} n_ot={cout} n_cbase={n_cbase}")
print(f">>> total (ot,cbase) tile invocations = {n_row_tiles*n_col_tiles*cout*n_cbase}")
print(f">>> bundle written to {OUT_DIR}")
