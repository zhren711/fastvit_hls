"""
build_patchhoist_probe_nod_cout96.py -- A3 angle-B, n_ot decomposition probe
(ZHR-92, 2026-08-24). Tests whether the unexplained per-tile residual R
(164.8us/tile from the cin=32/64 regression) is actually per-tile or
secretly per-ot: cin=48 held fixed (so PW_PATCH_HOIST and n_cbase are
UNCHANGED from the baseline), cout doubled 48->96 (n_ot doubles), spatial
size h_in=w_in=64 (256 tiles, same as the existing tile-count-scaling
probe's 256-tile baseline point, 102.42ms/256=400.1us/tile).

If R scales with n_ot: this run's total time should be roughly baseline +
256 tiles * (extra 48 ot's worth of R/48 each) -- i.e. total should grow
substantially beyond just the known flat-pipeline delta for doubling cout
(doubling n_ot roughly doubles pw_flat_pipeline's own total_iters too, so
some growth is expected regardless -- the diagnostic is whether growth
EXCEEDS what the flat-pipeline compute-only model predicts).
If R is a genuine per-tile constant: growth should track close to what the
flat-pipeline II=3 model alone predicts for the larger total_iters, since
PW_PATCH_HOIST (cin-only) and R (claimed tile-only) are both unchanged.
"""
import struct
import os

ROOT = r"E:\codes\microzed\fastvit_hls"
OUT_DIR = os.path.join(ROOT, "accuracy_test_imgs_256", "board_test_patchhoist_nod_cout96")
os.makedirs(OUT_DIR, exist_ok=True)

MAC_PR, MAC_PC, MAC_PD, MAX_CIN_PW = 4, 4, 2, 32

OP_PWCONV = 1
cin, cout = 48, 96
h_in, w_in = 64, 64
k, stride, pad, fpg = 1, 1, 0, 1
out_shift = 0

in_buf = bytes([1]) * (cin * h_in * w_in)           # 196,608 bytes, all 1
w_buf = bytes([1]) * (cout * cin * k * k)            # 4,608 bytes, all 1
b_buf = struct.pack("<%di" % cout, *([0] * cout))    # 384 bytes, all 0

h_out = (h_in + 2 * pad - k) // stride + 1
w_out = (w_in + 2 * pad - k) // stride + 1
assert h_out == 64 and w_out == 64
n_row_tiles = (h_out + MAC_PR - 1) // MAC_PR
n_col_tiles = (w_out + MAC_PC - 1) // MAC_PC
n_ch_tiles = (cin + MAC_PD - 1) // MAC_PD
last_row_tile = h_out - (n_row_tiles - 1) * MAC_PR
last_col_tile = w_out - (n_col_tiles - 1) * MAC_PC
last_ch_tile = cin - (n_ch_tiles - 1) * MAC_PD
in_ch_stride = h_in * w_in
out_ch_stride = h_out * w_out
n_cbase = (cin + MAX_CIN_PW - 1) // MAX_CIN_PW
assert n_row_tiles == 16 and n_col_tiles == 16 and n_cbase == 2 and cout == 96
print(f">>> total tiles = {n_row_tiles * n_col_tiles} (expect 256), n_cbase={n_cbase} (expect 2, same as baseline)")

ref_out = bytes([48]) * (cout * h_out * w_out)  # sum_{ci=0..47}(1*1)=48, hand-computed

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
